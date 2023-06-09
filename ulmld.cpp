#include <algorithm>
#include <cassert>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

// POSIX
#include <fcntl.h>
#include <unistd.h>

#include "archive-reader.hpp"

class Exception : public std::exception
{
  private:
    bool showAddress;
    std::uint64_t address;
    std::string msg;
    std::shared_ptr<Exception> nested;
    mutable std::string msgbuf; // temporarily used by what()
  public:
    Exception()
      : showAddress(false)
      , address(0)
    {
    }

    Exception(std::uint64_t address, std::string msg)
      : showAddress(true)
      , address(address)
      , msg(msg)
    {
    }

    Exception(std::uint64_t address, std::string msg, const Exception &nested)
      : showAddress(true)
      , address(address)
      , msg(msg)
      , nested(std::make_shared<Exception>(nested))
    {
    }

    Exception(std::string msg)
      : showAddress(false)
      , address(0)
      , msg(msg)
    {
    }

    Exception(std::string msg, const Exception &nested)
      : showAddress(false)
      , address(0)
      , msg(msg)
      , nested(std::make_shared<Exception>(nested))
    {
    }

    virtual ~Exception() throw() {}

    virtual const char *
    what() const throw()
    {
	std::ostringstream os;
	if (nested) {
	    os << nested->what() << std::endl;
	}
	if (showAddress) {
	    os << "[0x" << std::hex << std::setfill('0') << std::setw(16)
	       << address << "] ";
	}
	os << msg;

	msgbuf = os.str();
	return msgbuf.c_str();
    }
};

//------------------------------------------------------------------------------

template<typename T1, typename T2>
T1
alignAddr(T1 addr, T2 alignTo)
{
    return ((addr + alignTo - 1) / alignTo) * alignTo;
}

struct Segment
{
    Segment()
      : alignment(1)
      , baseAddr(0)
      , fill(0xFD)
    {
    }

    void
    setAlignment(std::uint64_t alignment_)
    {
	assert(baseAddr % alignment_ == 0);

	if (alignment_ > alignment) {
	    alignment = alignment_;
	}
	std::size_t newSize = alignAddr(size(), alignment);
	advanceTo(newSize);
    }

    void
    setMark(std::string filename)
    {
	mark[filename] = size();
    }

    std::uint64_t
    getMark(const std::string &filename)
    {
	return baseAddr + mark[filename];
    }

    std::uint64_t
    isAtMark(const std::string &filename)
    {
	return mark[filename] == size();
    }

    void
    advanceTo(std::uint64_t addr)
    {
	addr -= baseAddr;

	// after advanceTo(addr) the next byte will be appended at addr
	assert(addr >= size());

	std::size_t oldSize = size();

	while (addr > size()) {
	    appendByte(fill);
	}

	if (size() != oldSize) {
	    appendAnnotation("      (ulmld: padding for alignment)");
	}
	assert(size() == addr);
    }

    void
    appendByte(unsigned char byte)
    {
	memory[size()] = byte;
    }

    void
    patchBytes(std::uint64_t addr, std::uint64_t numBytes, std::uint64_t value)
    {
	addr -= baseAddr;

	for (std::uint64_t i = numBytes; i-- > 0;) {
	    unsigned char byte = value & 0xFF;
	    value >>= 8;
	    memory[addr + i] = byte;
	}
    }

    bool
    requiresAdvanceTo(std::uint64_t addr)
    {
	return addr - baseAddr > size();
    }

    void
    insertByteString(std::uint64_t addr, std::string hexDigits)
    {
	addr -= baseAddr;

	if (requiresAdvanceTo(addr)) {
	    advanceTo(addr);
	}

	std::size_t numBytes = hexDigits.length();
	assert(numBytes % 2 == 0);
	numBytes /= 2;

	for (std::size_t i = 0; i < numBytes; ++i) {
	    std::string byteString;
	    int byte;

	    byteString = hexDigits.substr(0, 2);
	    hexDigits = hexDigits.substr(2);

	    std::istringstream in(byteString);
	    if (!(in >> std::hex >> byte)) {
		std::cerr << "not in hex format or corrupted " << std::endl;
	    }
	    memory[addr + i] = byte & 0xFF;
	}
    }

    void
    appendAnnotation(const std::string &text)
    {
	std::size_t addr = size() > 0 ? size() - 1 : 0;

	insertAnnotation(text, baseAddr + addr);
    }

    void
    insertAnnotation(const std::string &text, std::size_t addr)
    {
	addr -= baseAddr;

	if (annotation.count(addr)) {
	    annotation[addr] += ", ";
	} else {
	    annotation[addr] += "# ";
	}
	annotation[addr] += text;
    }

    void
    insertLabel(const std::string &text, std::size_t addr)
    {
	label[addr - baseAddr].push_back(text);
    }

    void
    appendHeader(const std::string &text)
    {
	header[size()].push_back(text);
    }

    std::uint64_t
    size() const
    {
	return memory.size();
    }

    void
    print(std::ostream &out, bool strip = false) const
    {
	for (std::uint64_t i = 0; i < size(); ++i) {
	    if (!strip) {
		if (header.count(i)) {
		    for (auto &v : header.at(i)) {
			out << v << std::endl;
		    }
		}
		if (label.count(i)) {
		    for (auto &v : label.at(i)) {
			out << v << std::endl;
		    }
		}
		out << "0x" << std::setw(16) << std::setfill('0') << std::hex
		    << std::uppercase << (i + baseAddr) << ": ";
		std::uint64_t addr = i + baseAddr;
		if (addr % 4 != 0) {
		    out << std::setw(3 * (addr % 4)) << std::setfill(' ')
			<< " ";
		}
	    }
	    // print remaining bytes till next annotation
	    for (; i < size(); ++i) {
		out << std::setw(2) << std::setfill('0') << std::hex
		    << std::uppercase << int(memory.at(i))
		    << (strip ? "" : " ");
		if (!strip) {
		    std::uint64_t addr = i + baseAddr;
		    if (annotation.count(i)) {
			if (addr % 4 != 3) {
			    out << std::setw(3 * (3 - addr % 4))
				<< std::setfill(' ') << " ";
			}
			out << annotation.at(i) << std::endl;
			break;
		    }
		    if (header.count(i + 1) || label.count(i + 1)) {
			out << std::endl;
			break;
		    }
		    if (addr % 4 == 3) {
			out << std::endl
			    << std::setw(20) << std::setfill(' ') << " ";
		    }
		}
	    }
	}
	if (!annotation.count(size() - 1)) {
	    out << std::endl;
	}
	if (header.count(size())) {
	    for (auto &v : header.at(size())) {
		out << v << std::endl;
	    }
	}
    }

    void
    setBaseAddr(std::uint64_t baseAddr_)
    {
	assert(baseAddr_ % alignment == 0);

	baseAddr = baseAddr_;
    }

    std::uint64_t
    getEndAddr()
    {
	return baseAddr + size();
    }

    std::uint64_t alignment, baseAddr;
    unsigned char fill;
    std::map<std::uint64_t, unsigned char> memory;
    std::map<std::uint64_t, std::string> annotation;
    std::map<std::uint64_t, std::vector<std::string>> header, label;
    std::map<std::string, std::uint64_t> mark;
};

struct ObjectFile
{
    static constexpr std::size_t numSegments = 3;

    ObjectFile()
      : segments(numSegments)
    {
	if (std::getenv("ULM_LIBRARY_PATH")) {
	    std::string libpath_env = std::getenv("ULM_LIBRARY_PATH");

	    size_t pos = 0;
	    std::string delim = ":";
	    while ((pos = libpath_env.find(delim)) != std::string::npos) {
		libpath.insert(libpath_env.substr(0, pos));
		libpath_env.erase(0, pos + delim.length());
	    }
	    libpath.insert(libpath_env);

	    /*
	    std::cout << "--------" << std::endl;
	    for (auto p : libpath) {
		std::cout << p << std::endl;
	    }
	    std::cout << "--------" << std::endl;
	    */
	}
    }

    using SymEntry = std::pair<char, std::uint64_t>;

    struct FixEntry
    {
	FixEntry(std::string segment, std::uint64_t addr, std::uint64_t offset,
		 std::uint64_t numBytes, std::string kind,
		 std::int64_t displace)
	  : segment(segment)
	  , kind(kind)
	  , addr(addr)
	  , offset(offset)
	  , numBytes(numBytes)
	  , displace(displace)
	{
	}

	std::string segment, kind;
	std::uint64_t addr, offset, numBytes;
	std::int64_t displace;
    };

    std::optional<std::string>
    readSymtabIndex(std::istream &in)
    {
	std::string line;
	while (std::getline(in, line)) {
	    char kind;
	    std::string ident, member;

	    std::istringstream(line) >> kind >> ident >> member;
	    if (unresolved.count(ident)) {
		return member;
	    }
	}
	return std::nullopt;
    }

    /*
	return value:
	0  complete object file or all object files of a library were added.
	1  one object file from a library was added to solve a unresolved
       symbol.
    */

    int
    addLibOrObject(std::string file, bool onlyLibs = false)
    {
	ar::archive_reader archive;

	bool success = false;
	if (file.find("-l") == 0) {
	    for (auto path : libpath) {
		path = path + "/lib" + file.substr(2) + ".a";
		if (archive.open(path.c_str())) {
		    success = true;
		    file = path;
		    break;
		}
	    }
	} else {
	    if (archive.open(file.c_str())) {
		success = true;
	    }
	}
	if (!success && !onlyLibs) {
	    std::ifstream in(file);
	    if (!in) {
		std::ostringstream os;
		if (file.find("-l") == 0) {
		    os << "can not find " << file;
		} else {
		    os << "can not open " << file;
		}
		throw Exception(os.str());
	    }
	    readSegments(in, file);
	    return 0;
	}

	int resolved = 0;

	ar::archive_stream in(archive);
	in.open("__SYMTAB_INDEX");
	if (!in) {
	    for (auto &member : archive) {
		in.open(member.name);
		std::string name = file + "(" + member.name + ")";
		readSegments(in, name);
	    }
	} else {
	    while (1) {
		if (auto member = readSymtabIndex(in)) {
		    in.open(*member);
		    std::string name = file + "(" + *member + ")";
		    readSegments(in, name);
		    resolved = 1;
		    in.open("__SYMTAB_INDEX");
		    continue;
		}
		break;
	    }
	}
	return resolved;
    }

    void
    readSegments(std::istream &in, std::string source)
    {
	std::string line, comment;
	std::uint64_t addr, baseAddr = 0;
	std::size_t seg = -1;

	if (in.peek() != '#') {
	    std::ostringstream os;
	    os << "not an object file " << source;
	    throw Exception(os.str());
	}

	while (std::getline(in, line)) {
	    if (line.find("#TEXT") == 0) {
		seg = 0;
		line = line.substr(5);
		line.erase(std::remove_if(line.begin(), line.end(), isspace),
			   line.end());
		if (line.length() > 0) {
		    std::uint64_t alignment;
		    std::istringstream iss(line);
		    std::istringstream(line) >> alignment;
		    segments[seg].setAlignment(alignment);
		}
		segments[seg].setMark(source);
		continue;
	    }
	    if (line.find("#DATA") == 0) {
		seg = 1;
		line = line.substr(5);
		line.erase(std::remove_if(line.begin(), line.end(), isspace),
			   line.end());
		if (line.length() > 0) {
		    std::uint64_t alignment;
		    std::istringstream(line) >> alignment;
		    segments[seg].setAlignment(alignment);
		}
		segments[seg].setMark(source);
		continue;
	    }
	    if (line.find("#BSS") == 0) {
		seg = 2;
		segments[seg].setMark(source);
		line = line.substr(4);
		assert(line.length());

		std::uint64_t alignment, size;
		std::istringstream(line) >> std::dec >> alignment >> size;

		segments[seg].setAlignment(alignment);
		if (size) {
		    size += segments[seg].getMark(source);
		    segments[seg].advanceTo(size);
		}
		continue;
	    }
	    if (line.find("#SYMTAB") == 0) {
		seg = 3;
		continue;
	    }
	    if (line.find("#FIXUPS") == 0) {
		seg = 4;
		continue;
	    }
	    if (line.find("#") == 0 || line.length() == 0) {
		continue;
	    }
	    // reading text or data segement
	    if (seg == 0 || seg == 1) {
		// extract comment (if any)
		std::size_t comment_index = line.find("#");
		if (comment_index != std::string::npos) {
		    ++comment_index; // skip '#'
		    if (line[comment_index] == ' ') {
			++comment_index;
		    }
		    comment = line.substr(comment_index);
		} else {
		    comment = "";
		}

		// remove comment and spaces
		line.erase(std::find(line.begin(), line.end(), '#'),
			   line.end());
		line.erase(std::remove_if(line.begin(), line.end(), isspace),
			   line.end());

		if (segments[seg].isAtMark(source)) {
		    segments[seg].appendHeader("# from: " + source);
		}

		// extract address (if any)
		if (line.find(":") != std::string::npos) {
		    std::istringstream(line) >> std::hex >> addr;
		    line = line.substr(line.find(":") + 1);

		    if (segments[seg].isAtMark(source)) {
			baseAddr = addr;
		    }
		    addr -= baseAddr;
		} else {
		    addr = segments[seg].size() - segments[seg].getMark(source);
		    if (segments[seg].isAtMark(source)) {
			baseAddr = addr;
		    }
		}

		addr += segments[seg].getMark(source);
		if (segments[seg].requiresAdvanceTo(addr)) {
		    std::ostringstream os;
		    os << "In segment '" << seg
		       << "' (0=text, 1=data, "
			  "2=bss) there is a gap that would require "
			  "fillin bytes. That's only allowed for "
			  "alignment";
		    throw Exception(os.str());
		}
		segments[seg].insertByteString(addr, line);
		if (comment.length()) {
		    segments[seg].appendAnnotation(comment);
		}
		continue;
	    }
	    // reading symtab
	    if (seg == 3) {
		char kind;
		std::string ident;
		std::uint64_t value;

		std::istringstream(line) >> kind >> ident >> std::hex >> value;

		switch (kind) {
		    case 'T':
			unresolved.erase(ident);
		    case 't':
			value += segments[0].getMark(source);
			segments[0].insertLabel("#" + ident + ":", value);
			break;
		    case 'D':
			unresolved.erase(ident);
		    case 'd':
			value += segments[1].getMark(source);
			segments[1].insertLabel("#" + ident + ":", value);
			break;
		    case 'B':
			unresolved.erase(ident);
		    case 'b':
			value += segments[2].getMark(source);
			segments[2].insertLabel("#" + ident + ":", value);
			break;
		}
		if (kind == 'U') {
		    if (!symTab.count(ident) || !isupper(symTab[ident].first)) {
			unresolved.insert(ident);
		    }
		    continue;
		}

		if (ident[0] == '.') {
		    continue;
		}
		if (std::toupper(kind) != kind) {
		    localSymTab[ident].push_back({ kind, value });
		    continue;
		}
		if (symTab.count(ident)) {
		    std::ostringstream os;
		    os << " multiple definition of `" << ident;
		    throw Exception(os.str());
		}
		symTab[ident] = { kind, value };
		continue;
	    }
	    // reading fixables
	    if (seg == 4) {
		std::string segment, kind, ident;
		std::uint64_t address, offset, numBytes;

		std::istringstream(line) >> segment >> std::hex >> address >>
		  std::dec >> offset >> numBytes >> kind >> ident;

		// hack to support ulmas for ulm-generator
		assert(offset % 8 == 0);
		assert(numBytes % 4 == 0);
		offset /= 8;
		numBytes /= 8;

		std::size_t fixInSeg = segment == "text" ? 0 : 1;
		std::int64_t displace = 0;

		address += segments[fixInSeg].getMark(source);

		if (std::size_t p = ident.find("+"); p != std::string::npos) {
		    std::istringstream(ident.substr(p)) >> displace;
		    ident = ident.substr(0, p);
		} else if (std::size_t p = ident.find("-");
			   p != std::string::npos) {
		    std::istringstream(ident.substr(p)) >> displace;
		    ident = ident.substr(0, p);
		}

		if (ident == "[text]") {
		    displace += segments[0].getMark(source);
		} else if (ident == "[data]") {
		    displace += segments[1].getMark(source);
		} else if (ident == "[bss]") {
		    displace += segments[2].getMark(source);
		}

		fixables[ident].push_back(
		  FixEntry(segment, address, offset, numBytes, kind, displace));

		continue;
	    }
	}
    }

    void
    printSegment(std::ostream &out, int seg, bool printAddr = true) const
    {
	if (segments[seg].size()) {
	    segments[seg].print(out, printAddr);
	}
    }

    void
    print(std::ostream &out, const std::string &ulm, bool strip = false) const
    {
	out << "#!/usr/bin/env -S " << ulm << std::endl;
	out << "#TEXT " << std::dec << segments[0].alignment << std::endl;
	printSegment(out, 0, strip);
	out << "#DATA " << std::dec << segments[1].alignment << std::endl;
	printSegment(out, 1, strip);
	out << "#BSS " << segments[2].alignment << " " << std::dec
	    << segments[2].size() << std::endl
	    << "#(begins at 0x" << std::hex << segments[2].baseAddr << ")"
	    << std::endl;

	out << "#SYMTAB " << std::endl;
	for (auto &[k, e] : symTab) {
	    out << e.first << " " << std::left << std::setw(27)
		<< std::setfill(' ') << k << " 0x" << std::right
		<< std::setw(16) << std::setfill('0') << std::hex
		<< std::uppercase << e.second << std::endl;
	}
	for (auto &[k, v] : localSymTab) {
	    for (auto &e : v) {
		out << e.first << " " << std::left << std::setw(27)
		    << std::setfill(' ') << k << " 0x" << std::right
		    << std::setw(16) << std::setfill('0') << std::hex
		    << std::uppercase << e.second << std::endl;
	    }
	}
    }

    void
    dumpUnresolved()
    {
	for (auto &ident : unresolved) {
	    std::cout << ident << std::endl;
	}
    }

    void
    link()
    {
	auto textAddr = segments[0].baseAddr;

	// adjust data segment
	auto dataAddr =
	  alignAddr(segments[0].getEndAddr(), segments[1].alignment);
	segments[1].setBaseAddr(dataAddr);

	// fill the gap between text and data segement if necessary
	segments[0].advanceTo(dataAddr);

	// adjust bss segment
	auto bssAddr =
	  alignAddr(segments[1].getEndAddr(), segments[2].alignment);
	segments[2].setBaseAddr(bssAddr);

	// salami:
	// std::cerr << "textAddr = " << textAddr << std::endl;
	// std::cerr << "dataAddr = " << dataAddr << std::endl;
	// std::cerr << "bssAddr = " << bssAddr << std::endl;

	// update in symtap relative addresses
	for (auto &[ident, e] : symTab) {
	    if (e.first == 'T') {
		e.second += textAddr;
	    } else if (e.first == 'D') {
		e.second += dataAddr;
	    } else if (e.first == 'B') {
		e.second += bssAddr;
	    } else if (e.first != 'A') {
		std::ostringstream os;
		os << "Can't handle symTab kind '" << e.first
		   << "' in this case";
		throw Exception(os.str());
	    }
	}

	// resolve fixables
	for (auto &[ident, vFixEntry] : fixables) {
	    for (auto &fixEntry : vFixEntry) {
		std::uint64_t addr = fixEntry.addr;
		std::size_t seg;

		if (fixEntry.segment == "text") {
		    addr += textAddr;
		    seg = 0;
		} else if (fixEntry.segment == "data") {
		    addr += dataAddr;
		    seg = 1;
		} else {
		    std::ostringstream os;
		    os << "Can't apply a fix in segment " << fixEntry.segment;
		    throw Exception(os.str());
		}

		std::uint64_t value = fixEntry.displace;

		if (ident == "[text]") {
		    value += textAddr;
		} else if (ident == "[data]") {
		    value += dataAddr;
		} else if (ident == "[bss]") {
		    value += bssAddr;
		} else if (symTab.count(ident)) {
		    value += symTab[ident].second;
		} else {
		    std::ostringstream os;
		    os << "Unresolved symbol " << ident;
		    throw Exception(os.str());
		}

		if (fixEntry.kind == "relative") {
		    if ((value - addr) % 4 != 0) {
			std::ostringstream os;
			os << "address for relative jump is not a multiple of "
			      "4 ";
			throw Exception(os.str());
		    }

		    value = (value - addr) / 4;
		} else if (fixEntry.kind == "w0") {
		    value = value & 0xFFFF;
		} else if (fixEntry.kind == "w1") {
		    value = value >> 16 & 0xFFFF;
		} else if (fixEntry.kind == "w2") {
		    value = value >> 32 & 0xFFFF;
		} else if (fixEntry.kind == "w3") {
		    value = value >> 48 & 0xFFFF;
		} else if (fixEntry.kind != "absolute") {
		    std::ostringstream os;
		    os << "Can not apply a '" << fixEntry.kind << "' fix.";
		    throw Exception(os.str());
		}

		segments[seg].patchBytes(addr + fixEntry.offset,
					 fixEntry.numBytes, value);
	    }
	}
    }

    std::vector<Segment> segments;
    std::map<std::string, SymEntry> symTab;
    std::map<std::string, std::vector<SymEntry>> localSymTab;
    std::set<std::string> unresolved;
    std::map<std::string, std::vector<FixEntry>> fixables;
    std::set<std::string> libpath;

    /*
	ident -> [ { segment, addr, offset, num, kind, displace } ]
    */
};

static std::vector<std::string> executables;

void
delete_executable()
{
    for (auto &f : executables) {
	std::remove(f.c_str());
    }
}

std::unique_ptr<std::ofstream>
open_executable(const char *filename)
{
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (!fd) {
	std::cerr << "cannot create '%s'" << filename << std::endl;
	std::exit(1);
    }
    close(fd);
    executables.push_back(filename);
    return std::make_unique<std::ofstream>(filename);
}

static const char *cmdname;

void
usage()
{
    std::cerr << "usage: " << cmdname << " [options] file..." << std::endl;
    exit(1);
}

int
main(int argc, char **argv)
{
    std::unique_ptr<std::ostream> pOut;
    std::vector<std::string> inFile;
    std::uint64_t startAddr = 0;
    ObjectFile objectFile;

    cmdname = *argv++;
    --argc;

    if (argc < 1) {
	usage();
    }

#   include "call_start.hpp"

    std::optional<int> startGroup;

    try {
	for (int i = 0; i < argc; ++i) {
	    if (!strcmp("-L", argv[i])) {
		if (++i >= argc) {
		    usage();
		}
		objectFile.libpath.insert(argv[i]);
		continue;
	    }
	    if (!strncmp("-L", argv[i], 2)) {
		objectFile.libpath.insert(argv[i] + 2);
		continue;
	    }
	}

	for (int i = 0; i < argc; ++i) {
	    if (!strcmp("-o", argv[i])) {
		pOut = open_executable(argv[++i]);
		continue;
	    }
	    if (!strcmp("-textseg", argv[i])) {
		std::istringstream in(argv[i + 1]);
		in >> std::hex >> startAddr;
		continue;
	    }
	    if (!strcmp("-L", argv[i])) {
		++i;
		continue;
	    }
	    if (!strncmp("-L", argv[i], 2)) {
		continue;
	    }
	    if (!strcmp("--start-group", argv[i]) || !strcmp("-(", argv[i])) {
		startGroup = i + 1;
		continue;
	    }
	    if (!strcmp("--end-group", argv[i]) || !strcmp("-)", argv[i])) {
		if (!startGroup.has_value()) {
		    std::cerr << cmdname << ": missing --start-group or -("
			      << std::endl;
		    return 1;
		}
		do {
		    int resolved = 0;
		    for (int g = startGroup.value(); g < i; ++g) {
			resolved +=
			  objectFile.addLibOrObject(argv[g], true) > 0;
		    }
		    if (!resolved) {
			break;
		    }
		} while (true);
		//startGroup.reset(); TODO: gcc bug? Gives warning ...
		continue;
	    }
	    objectFile.addLibOrObject(argv[i]);
	}
	if (startGroup.has_value()) {
	    std::cerr << cmdname
		      << ": --start-group not terminated with --end-group"
		      << std::endl;
	    return 1;
	}

	// std::ostream    &out = pOut ? *pOut : std::cout;
	if (!pOut) {
	    pOut = open_executable("a.out");
	}
	std::ostream &out{ *pOut };
	objectFile.link();
	objectFile.print(out, ulm, false);
    } catch (Exception &e) {
	delete_executable();
	std::cerr << cmdname << ": execution aborted" << std::endl
		  << e.what() << std::endl;
	std::exit(1);
    }
}
