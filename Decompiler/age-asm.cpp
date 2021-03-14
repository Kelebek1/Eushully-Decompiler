#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <array>

#include "disassembler.h"
#include "reassembler.h"

template<typename T>
inline void update_max(std::atomic<T>& atom, const T val) {
    for (T atom_val = atom;
        atom_val < val &&
        !atom.compare_exchange_strong(atom_val, val, std::memory_order_seq_cst);
        );
}
static std::atomic<size_t> max_val;

const std::size_t NUM_THREADS = std::max(std::thread::hardware_concurrency(), 4U);
void doDisassemble();
void doAssemble();
void doCheckFile();
void CheckFile(const std::filesystem::path& input);

static std::vector<std::pair<const std::filesystem::path, const std::filesystem::path>> files;

int main(s32 argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "AGE script utilities by Maide\n");
        fprintf(stderr, "Originally written by Kellindil\n\n");
        fprintf(stderr, "Usage: %s [-da] infile [outfile]\n", argv[0]);
        return -1;
    }

    std::filesystem::path input(argv[2]);
    std::filesystem::path output;

    if (strcmp(argv[1], "-x") == 0) {
        // For debugging. Reads a file, disassembles it, reassembles, 
        // and checks that the original and reassembled are binary identical
        if (std::filesystem::is_directory(input)) {
            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".bin" ||
                    file.path().extension() == ".BIN" || 
                    file.path().extension() == ".txt" || 
                    file.path().extension() == ".TXT") {
                    files.emplace_back(file, std::filesystem::path{});
                }
            }

            // Use only 1 thread for this
            for (const auto& file : files) {
                doCheckFile();
            }
        } else {
            CheckFile(input);
        }

    } else if (strcmp(argv[1], "-d") == 0) {
        // Dissassemble
        if (std::filesystem::is_directory(input)) {
            if (argc > 3) {
                output = std::string(argv[3]) + "/";
            } else {
                output = "decompiled/";
            }

            if (!std::filesystem::is_directory(output)) {
                std::filesystem::create_directories(output);
            }

            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".bin" ||
                    file.path().extension() == ".BIN") {
                    output.replace_filename(file.path().filename());
                    output.replace_extension(".txt");
                    files.emplace_back(file, output);
                }
            }
        } else {
            if (argc > 3) {
                output = argv[3];
            } else {
                output = input;
            }
            //std::cout << "Single file, in: " << input.string() << " out: " << output.string() << "\n";
            files.emplace_back(input, std::move(output));
        }

        auto start = std::chrono::system_clock::now();

        std::vector<std::thread> threads;
        for (u32 i = 0; i < std::min(NUM_THREADS, files.size()); i++) {
            threads.emplace_back(doDisassemble);
        }

        for (auto& thread : threads) {
            thread.join();
        }

        auto end = std::chrono::system_clock::now();
        std::cout << "Disassembly took " << (float)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000 << "s on " << std::thread::hardware_concurrency() << " cores." << '\n';

    } else if (strcmp(argv[1], "-a") == 0) {
        // Assemble
        if (std::filesystem::is_directory(input)) {
            if (argc > 3) {
                output = std::string(argv[3]) + "/";
            } else {
                output = "compiled/";
            }

            if (!std::filesystem::is_directory(output)) {
                std::filesystem::create_directories(output);
            }

            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".txt") {
                    output.replace_filename(file.path().filename());
                    output.replace_extension(".BIN");
                    files.emplace_back(file, output);
                }
            }
        } else {
            if (argc > 3) {
                output = argv[3];
            } else {
                output = input;
            }
            files.emplace_back(input, std::move(output));
        }

        auto start = std::chrono::system_clock::now();
        
        std::vector<std::thread> threads;
        for (u32 i = 0; i < std::min(NUM_THREADS, files.size()); ++i) {
            threads.emplace_back(doAssemble);
        }

        for (auto& thread : threads) {
            thread.join();
        }
        
        auto end = std::chrono::system_clock::now();

        std::cout << "Assembly took " << (float)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000 << "s on " << std::thread::hardware_concurrency() << " cores." << '\n';
    } else {
        fprintf(stderr, "Unknown option : %s\n", argv[1]);
        return -1;
    }
    return 0;
}

void doDisassemble() {
    static std::atomic<u32> front;

    while (front < files.size()) {
        auto& [input, output] = files[front++];

        fprintf(stdout, "Disassembling %s into %s\n", input.string().c_str(), output.string().c_str());

        std::ifstream fd_in(input, std::ios::in | std::ios::binary);
        if (!fd_in.is_open()) {
            fprintf(stderr, "Unable to open %s, skipping.\n", input.string().c_str());
            continue;
        }
        std::stringstream fd{ disassemble(fd_in) };
        std::ofstream fd_out(output, std::ios::out | std::ios::binary);
        fd_out.write(fd.str().data(), fd.str().length());
    }
}

void doAssemble() {
    static std::atomic<u32> front;

    while (front < files.size()) {
        auto& [input, output] = files[front++];

        fprintf(stdout, "Assembling %s into %s\n", input.string().c_str(), output.string().c_str());

        std::ifstream fd_in(input, std::ios::in);
        if (!fd_in.is_open()) {
            fprintf(stderr, "Unable to open %s, skipping.\n", input.string().c_str());
            continue;
        }
        std::stringstream fd{ assemble(fd_in) };
        std::ofstream fd_out(output, std::ios::out | std::ios::binary);
        fd_out.write(fd.str().data(), fd.str().length());
    }
}


bool compareFile(std::istream& f1, std::istream& f2) {
    // Read 128Kb at a time
    static std::array<char, 131'072> buf1, buf2;
    do {
        f1.read(buf1.data(), buf1.size());
        f2.read(buf2.data(), buf2.size());
        size_t r1 = f1.gcount();
        size_t r2 = f2.gcount();
        if (r1 != r2 || std::memcmp(buf1.data(), buf2.data(), r1)) {
            return false;
        }
    } while (!f1.eof() && !f2.eof());

    return f1.eof() && f2.eof();
}

void CheckFile(const std::filesystem::path& input) {
    std::ifstream fd_in;
    std::stringstream file1, file2;

    if (input.extension() == ".bin" || input.extension() == ".BIN") {
        fd_in = std::ifstream(input, std::ios::in | std::ios::binary);
        if (!fd_in.is_open()) {
            fprintf(stderr, "Unable to open %s.\n", input.string().c_str());
            exit(-1);
        }
        file1 = disassemble(fd_in);
        file1.clear();
        file1.seekg(0, std::ios::beg);
        file2 = assemble(file1);
    } else {
        fd_in = std::ifstream(input, std::ios::in);
        if (!fd_in.is_open()) {
            fprintf(stderr, "Unable to open %s.\n", input.string().c_str());
            exit(-1);
        }
        file1 = assemble(fd_in);
        file1.clear();
        file1.seekg(0, std::ios::beg);
        file2 = disassemble(file1);
    }

    fd_in.clear();
    fd_in.seekg(0, std::ios::beg);
    file2.clear();
    file2.seekg(0, std::ios::beg);

    bool equal = compareFile(fd_in, file2);

    if (!equal) {
        fprintf(stdout, "\tdifferent!\n");
        exit(-1);
    } else {
        fprintf(stdout, "\tequal\n");
    }
}

void doCheckFile() {
    static std::atomic<u32> front;

    while (front < files.size()) {
        auto& [input, output] = files[front++];
        
        fprintf(stdout, "Checking file %s\n", input.string().c_str());
        
        CheckFile(input);
    }
}