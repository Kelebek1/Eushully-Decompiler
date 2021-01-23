#include <string>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

#include "disassembler.h"
#include "reassembler.h"

const std::size_t NUM_THREADS = 6;
void doDisassemble(std::filesystem::path output, bool isSingleFile);
void doAssemble(std::filesystem::path output, bool isSingleFile);

const u32 N = 100'000;
static char* buf1 = new char[N];
static char* buf2 = new char[N];

bool compareFile(std::ifstream& f1, std::ifstream& f2) {
  do {
    f1.read(buf1, N);
    size_t r1 = f1.gcount();
    f2.read(buf2, N);
    size_t r2 = f2.gcount();
    if (r1 != r2 ||    memcmp(buf1, buf2, r1)) {
        return false;  // Files are not equal
    }
  } while (!f1.eof() && !f2.eof());

  return f1.eof() && f2.eof();
}

void CheckFile(std::filesystem::path input) {
    fprintf(stdout, "Checking file %s\n", input.string().c_str());
    std::filesystem::path dis(input);
    dis.replace_extension(".txt");
    std::filesystem::path rea(input);
    rea.replace_extension(".rea");

    disassemble(input, dis);
    assemble(dis, rea);

    std::ifstream input_file(input, std::ios::binary);
    std::ifstream rea_file(rea, std::ios::binary);

    bool equal = compareFile(input_file, rea_file);

    input_file.close();
    rea_file.close();

    if (!equal) {
        // Keep the files for investigation on failing the comapre
        fprintf(stdout, "\tdifferent!\n");
        exit(-1);
    }
    else {
        fprintf(stdout, "\tequal\n");
    }

    std::filesystem::remove(dis);
    std::filesystem::remove(rea);
}

static std::vector<std::filesystem::path> files;

int main(s32 argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "AGE script utilities by Maide\n");
        fprintf(stderr, "Originally written by Kellindil\n\n");
        fprintf(stderr, "Usage: %s [-da] infile [outfile]\n", argv[0]);
        return -1;
    }

    std::filesystem::path input(argv[2]);
    std::filesystem::path output;

    // The following was used for debug. 
    // reads a file, disassembles it, reassembles, 
    // and checks that the original and reassembled are binary identical
    if (strcmp(argv[1], "-x") == 0) {
        if (std::filesystem::is_directory(input)) {
            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".bin" ||
                    file.path().extension() == ".BIN") {
                    CheckFile(file);
                }
            }
        }
        else {
            CheckFile(input);
        }

    } else if (strcmp(argv[1], "-d") == 0) {
        bool isSingleFile = false;

        if (std::filesystem::is_directory(input)) {
            if (argc > 3) {
                output = std::string(argv[3]) + "/";
            }
            else {
                output = "decompiled/";
            }

            if (!std::filesystem::is_directory(output)) {
                std::filesystem::create_directories(output);
            }

            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".bin" ||
                    file.path().extension() == ".BIN") {
                    files.push_back(file);
                }
            }
        } else {
            if (argc > 3) {
                output = argv[3];
            }
            else {
                output = input;
            }
            //std::cout << "Single file, in: " << input.string() << " out: " << output.string() << "\n";
            files.push_back(input);
            isSingleFile = true;
        }

        auto start = std::chrono::system_clock::now();
        std::filesystem::path out{ output };

        std::vector<std::thread> threads;
        for (u32 i = 0; i < std::min(NUM_THREADS, files.size()); i++)
            threads.emplace_back(std::thread(doDisassemble, out, isSingleFile));

        for (auto& thread : threads)
            thread.join();

        auto end = std::chrono::system_clock::now();
        std::cout << "Disassembly took " << (float)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000 << "s.\n";

    } else if (strcmp(argv[1], "-a") == 0) {
        bool isSingleFile = false;

        if (std::filesystem::is_directory(input)) {
            if (argc > 3) {
                output = std::string(argv[3]) + "/";
            }
            else {
                output = "compiled/";
            }

            if (!std::filesystem::is_directory(output)) {
                std::filesystem::create_directories(output);
            }

            for (auto& file : std::filesystem::directory_iterator(input)) {
                if (file.path().extension() == ".txt") {
                    files.push_back(file);
                }
            }
        } else {
            if (argc > 3) {
                output = argv[3];
            }
            else {
                output = input;
            }
            files.push_back(output);
            isSingleFile = true;
        }

        auto start = std::chrono::system_clock::now();
        std::filesystem::path out{ output };

        std::vector<std::thread> threads;
        for (u32 i = 0; i < std::min(NUM_THREADS, files.size()); ++i)
            threads.emplace_back(std::thread(doAssemble, output, isSingleFile));

        for (auto& thread : threads)
            thread.join();

        auto end = std::chrono::system_clock::now();

        std::cout << "Assembly took " << (float)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000 << "s.\n";
    }
    else {
        fprintf(stderr, "Unknown option : %s\n", argv[1]);
        return -1;
    }
    return 0;
}

void doDisassemble(std::filesystem::path output, bool isSingleFile) {
    static std::atomic<u32> front;
    std::filesystem::path input;

    while (front < files.size()) {
        input = files[front++];

        if (!isSingleFile)
            output.replace_filename(input.filename());
        output.replace_extension(".txt");

        disassemble(input, output);
    }
}

void doAssemble(std::filesystem::path output, bool isSingleFile) {
    static std::atomic<u32> front;
    std::filesystem::path input;

    while (front < files.size()) {
        input = files[front++];

        if (!isSingleFile)
            output.replace_filename(input.filename());
        output.replace_extension(".BIN");

        assemble(input, output);
    }
}