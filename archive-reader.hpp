/*
   Copyright (c) 2020 Andreas F. Borchert
   All rights reserved.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
   KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/*
   This header-only C++11 package provides reading access to the
   so-called common portable archive format. The format is called
   portable as (in contrast to its predecessors) it avoids any
   dependencies to the endianness of the host system by representing
   all meta informations in a printable format. It is called common as
   it is widely used on Linux, BSD, and Solaris -- but with some minor
   variations.

   This package depends on the C++11 standard, the POSIX standard, and
   on the presence of <ar.h> which is not included in POSIX but widely
   present on systems that support the common portable archive format.
   (POSIX itself includes the ar utility but does not specify its
   format. Quote: "Archives are files with unspecified formats." However,
   the POSIX standard quotes in the rationale section the description
   of the archive format by the 4.4 BSD implementation.)

   Specifications of the common portable archive file format can
   be found:
    * https://docs.oracle.com/cd/E36784_01/html/E36873/ar.h-3head.html
    * https://www.freebsd.org/cgi/man.cgi?query=ar&sektion=5
   More infos:
    * https://pubs.opengroup.org/onlinepubs/9699919799/utilities/ar.html
    * https://en.wikipedia.org/wiki/Ar_(Unix)

   Archive readers can be opened using the open method or
   by passing the filename to the constructor:

    * Using the open method:

      using namespace ar;
      archive_reader archive;
      if (archive.open(archive_name)) {
	 // process archive
      } else {
	 // could not be opened (possibly not an archive)
      }

    * Passing the filename to the constructor:

      using namespace ar;
      archive_reader archive(archive_name);
      if (archive.is_open()) {
	 // process archive
      } else {
	 // could not be opened (possibly not an archive)
      }

   Archives can be scanned using an iterator. Special
   elements like the string table (for long filenames)
   or the symbol table (maintained by ranlib) will be
   skipped:

      for (auto& member: archive) {
	 fmt::printf("%6o %3u/%3u %10u %s\n",
	    member.mode,
	    member.uid, member.gid,
	    member.size,
	    member.name);
      }

   Note that member.name is of type std::string, i.e. you
   must not use std::printf instead of fmt::print.

   Individual members of an archive (including the symbol table)
   can be read using archive streams.

   Opening a regular member for reading:

      archive_stream in(archive);
      in.open(member_name);
      if (!in) {
	 // this member could not be opened
      }

   Opening the symbol table:

      archive_stream in(archive);
      in.open_symtable();
      if (!in) {
	 // there is no symbol table
      }
*/

#ifndef ARCHIVE_READER_HPP
#define ARCHIVE_READER_HPP

#if __cplusplus < 201103L
#error This file requires compiler and library support for the \
ISO C++ 2011 standard.
#else

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>

/* POSIX headers */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* non-standard headers */
#include <ar.h>

namespace ar {

namespace internal {

struct archive_header
{
    std::string name;
    bool is_string_table;
    unsigned int date;
    unsigned int uid;
    unsigned int gid;
    unsigned int mode;
    unsigned int size;

    template<std::size_t N>
    bool
    extract_value(const char (&s)[N], unsigned int base, unsigned int &value)
    {
	/* some archive header fields are large enough
	   to overflow an 32-bit unsigned value but not a 64-bit unsigned */
	unsigned long long int val = 0;
	bool skip = true;
	bool padding = false;
	for (std::size_t i = 0; i < N; ++i) {
	    char ch = s[i];
	    if (skip) {
		if (ch == ' ') {
		    continue;
		}
		skip = false;
	    }
	    if (padding) {
		if (ch != ' ') {
		    return false;
		}
		continue;
	    }
	    if (ch == ' ') {
		padding = true;
		continue;
	    }
	    if (ch < '0' || ch > '9') {
		return false;
	    }
	    unsigned int digit = ch - '0';
	    if (digit >= base) {
		return false;
	    }
	    val = val * base + digit;
	}
	if (skip) {
	    return false;
	}
	if (val > std::numeric_limits<unsigned int>::max()) {
	    return false;
	}
	value = static_cast<unsigned int>(val);
	return true;
    }

    template<std::size_t N>
    bool
    extract_offset(const char (&s)[N], unsigned int &value)
    {
	unsigned long long int val = 0;
	for (std::size_t i = 1; i < N; ++i) {
	    char ch = s[i];
	    if (ch == ' ') {
		if (i == 1) {
		    return false;
		}
		break;
	    }
	    if (ch < '0' || ch > '9') {
		return false;
	    }
	    unsigned int digit = ch - '0';
	    val = val * 10 + digit;
	}
	if (val > std::numeric_limits<unsigned int>::max()) {
	    return false;
	}
	value = static_cast<unsigned int>(val);
	return true;
    }

    bool
    scan(const struct ar_hdr *hdr, std::size_t string_table_len,
	 const char *string_table)
    {
	if (std::memcmp(hdr->ar_fmag, ARFMAG, sizeof(hdr->ar_fmag)) != 0) {
	    return false;
	}
	is_string_table = hdr->ar_name[0] == '/' && hdr->ar_name[1] == '/';
	if (is_string_table) {
	    name = "";
	    date = uid = gid = mode = 0;
	    return extract_value(hdr->ar_size, 10, size);
	} else {
	    if (hdr->ar_name[0] == '/' && hdr->ar_name[1] != ' ') {
		/* reference into string table */
		if (!string_table) {
		    /* no string table found (must be first member) */
		    return false;
		}
		unsigned int offset;
		if (!extract_offset(hdr->ar_name, offset)) {
		    return false;
		}
		if (offset >= string_table_len) {
		    return false;
		}
		std::size_t len = 0;
		for (std::size_t i = offset; i + 1 < string_table_len; ++i) {
		    if (string_table[i] == '/') {
			if (i == offset || string_table[i + 1] != '\n') {
			    return false;
			}
			len = i - offset;
			break;
		    }
		}
		if (len == 0) {
		    return false;
		}
		name.clear();
		name.append(string_table + offset, len);
	    } else {
		std::size_t i = 0;
		std::size_t blank = 0;
		for (; i < sizeof(hdr->ar_name); ++i) {
		    char ch = hdr->ar_name[i];
		    if (ch == '/') {
			break;
		    }
		    if (ch != ' ') {
			blank = 0;
		    } else if (!blank) {
			blank = i;
		    }
		}
		std::size_t namelen;
		if (i < sizeof(hdr->ar_name)) {
		    namelen = i;
		} else if (blank) {
		    /* no trailing '/', possibly BSD variant of ar format */
		    namelen = blank;
		} else if (i == 0) {
		    /* empty name is permitted for the archive symbol table */
		    namelen = 0;
		} else {
		    /* not properly terminated */
		    return false;
		}
		name.clear();
		name.append(hdr->ar_name, namelen);
	    }
	    return extract_value(hdr->ar_date, 10, date) &&
		   extract_value(hdr->ar_uid, 10, uid) &&
		   extract_value(hdr->ar_gid, 10, gid) &&
		   extract_value(hdr->ar_mode, 8, mode) &&
		   extract_value(hdr->ar_size, 10, size);
	}
    }
};

}

class archive_stream;

class archive_reader
{
    friend class archive_stream;

  public:
    class member
    {
	friend class archive_reader;
	friend class archive_stream;

      private:
	member(std::string name, time_t mtime, uid_t uid, gid_t gid,
	       mode_t mode, size_t size, const char *addr)
	  : name(name)
	  , mtime(mtime)
	  , uid(uid)
	  , gid(gid)
	  , mode(mode)
	  , size(size)
	  , addr(addr)
	{
	}

      public:
	std::string name;
	time_t mtime;
	uid_t uid;
	gid_t gid;
	mode_t mode;
	size_t size;

      private:
	const char *addr;
    };

  private:
    using directory = std::map<std::string, member>;
    using directory_it = directory::const_iterator;

  public:
    class iterator
    {
	using iterator_category = std::bidirectional_iterator_tag;
	using value_type = const member;
	using reference = value_type &;

      public:
	iterator() {}
	iterator(directory_it it)
	  : it(it)
	{
	}
	iterator &
	operator++()
	{
	    ++it;
	    return *this;
	}
	iterator
	operator++(int)
	{
	    iterator retval = *this;
	    ++it;
	    return retval;
	}
	bool
	operator==(iterator other) const
	{
	    return it == other.it;
	}
	bool
	operator!=(iterator other) const
	{
	    return !(*this == other);
	}
	reference
	operator*() const
	{
	    return it->second;
	}

      private:
	directory_it it;
    };

    archive_reader()
      : fd(-1)
      , addr(nullptr)
      , len(0)
      , symtable(nullptr)
      , symtable_len(0)
    {
    }
    archive_reader(const char *filename)
      : archive_reader()
    {
	open(filename);
    }

    ~archive_reader()
    {
	close();
    }

    bool
    is_open() const
    {
	return fd >= 0;
    }

    bool
    open(const char *filename)
    {
	close();
	int newfd = ::open(filename, O_RDONLY);
	if (newfd < 0)
	    return false;
	struct ::stat statbuf;
	if (::fstat(newfd, &statbuf) < 0) {
	    ::close(newfd);
	    return false;
	}
	if (!S_ISREG(statbuf.st_mode)) {
	    /* not a regular file */
	    ::close(newfd);
	    return false;
	}
	using stat_size = std::make_unsigned<decltype(statbuf.st_size)>::type;
	if (static_cast<stat_size>(statbuf.st_size) >
	    std::numeric_limits<std::size_t>::max())
	{
	    /* the file is too large to be mapped into
	       our virtual address space */
	    ::close(newfd);
	    return false;
	}
	std::size_t size = static_cast<std::size_t>(statbuf.st_size);
	void *p = ::mmap(0, size, PROT_READ, MAP_SHARED, newfd, 0);
	if (p == MAP_FAILED) {
	    /* most likely insufficient resources */
	    ::close(newfd);
	    return false;
	}
	if (std::memcmp(p, ARMAG, SARMAG) != 0) {
	    /* this is not an archive as the magic string was not found */
	    ::munmap(p, size);
	    ::close(newfd);
	    return false;
	}
	fd = newfd;
	addr = static_cast<char *>(p);
	len = size;
	if (!scan()) {
	    close();
	    return false;
	}
	return true;
    }

    void
    close()
    {
	if (fd >= 0 && len > 0) {
	    ::munmap(const_cast<char *>(addr), len);
	    ::close(fd);
	    fd = -1;
	    addr = nullptr;
	    len = 0;
	    symtable = nullptr;
	    symtable_len = 0;
	    members.clear();
	}
    }

    iterator
    begin() const
    {
	return members.cbegin();
    }

    iterator
    end() const
    {
	return members.cend();
    }

  private:
    bool
    scan()
    {
	internal::archive_header header;
	std::size_t string_table_len = 0;
	const char *string_table = nullptr;
	const char *cp = addr + SARMAG;
	while (cp + sizeof(struct ar_hdr) <= addr + len) {
	    if (!header.scan((struct ar_hdr *)cp, string_table_len,
			     string_table)) {
		return false;
	    }
	    const char *begin = cp + sizeof(struct ar_hdr);
	    if (header.is_string_table) {
		if (members.size() > 0 || string_table) {
		    return false;
		}
		string_table = begin;
		string_table_len = header.size;
	    } else if (header.name.size() == 0) {
		/* symbol table */
		if (symtable) {
		    return false;
		}
		symtable = begin;
		symtable_len = header.size;
	    } else {
		auto res = members.insert({ header.name,
					    {
					      header.name,
					      header.date,
					      header.uid,
					      header.gid,
					      static_cast<mode_t>(header.mode),
					      header.size,
					      begin,
					    } });
		if (!res.second) {
		    return false;
		}
	    }
	    cp += sizeof(struct ar_hdr) + header.size;
	    if (header.size % 2) {
		++cp;
	    }
	}
	return cp == addr + len;
    }

    int fd;
    /* beginning address and len of the mmap'ed archive file */
    const char *addr;
    std::size_t len;
    /* symbol table, if any */
    const char *symtable;
    std::size_t symtable_len;
    /* member directory */
    directory members;
};

class archive_stream;
class archive_streambuf : public std::basic_streambuf<char>
{
  public:
    archive_streambuf() {}

  private:
    friend class archive_stream;
    archive_streambuf(const char *addr, std::size_t len)
    {
	set(addr, len);
    }
    void
    set(const char *addr, std::size_t len)
    {
	/* unfortunately, neitherer 'const char' as
	   type parameter nor 'const char*' for the
	   reading buffer are supported; hence we
	   have to resort to const_cast */
	char *p = const_cast<char *>(addr);
	this->setg(p, p, p + len);
    }
};

class archive_stream : public std::basic_istream<char>
{
    friend class archive_reader;

  public:
    archive_stream(const archive_reader &reader)
      : basic_istream<char>(&buf)
      , reader(reader)
    {
    }

    void
    open(std::string name)
    {
	auto it = reader.members.find(name);
	if (it == reader.members.end()) {
	    setstate(failbit);
	    return;
	}
	clear();
	buf.set(it->second.addr, it->second.size);
    }

    void
    open_symtable()
    {
	if (!reader.symtable) {
	    setstate(failbit);
	    return;
	}
	clear();
	buf.set(reader.symtable, reader.symtable_len);
    }

  private:
    void
    set(const char *addr, std::size_t len)
    {
	buf.set(addr, len);
    }

    const archive_reader &reader;
    archive_streambuf buf;
};

} // namespace ar

#endif // of #if __cplusplus < 201103L #else ...
#endif // ARCHIVE_READER_HPP
