#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <printf.hpp>
#include "archive-reader.hpp"

int
main(int argc, char** argv)
{
    using namespace ar;

    const char *cmdname = *argv++; --argc;
    if (argc != 1) {
	fmt::printf(std::cerr, "Usage: %s archive\n", cmdname);
	std::exit(1);
    }

    archive_reader archive(*argv);
    if (archive.is_open()) {
	archive_stream in(archive);

	for (auto& member: archive) {
	    if (member.name == "__SYMTAB_INDEX") {
		continue;
	    }
	    in.open(member.name);

	    std::string line;
	    while (std::getline(in, line)) {
		if (line != "#SYMTAB") {
		    continue;
		}
		while (std::getline(in, line)) {
		    char	    kind;
		    std::string	    ident, addr;

		    std::istringstream(line) >> kind >> ident >> addr;
		    if (line == "#FIXUPS") {
			break;
		    }
		    if (std::isupper(kind) && (kind != 'U')) {
			std::cout << kind << " "
			    << std::setw (27) << std::left << ident << " "
			    << member.name << std::endl;
		    }
		}
		break;
	    }
	}
    } else {
	fmt::printf(std::cerr, "%s: could not open as archive: %s\n",
		    cmdname, *argv);
	std::exit(1);
    }
}
