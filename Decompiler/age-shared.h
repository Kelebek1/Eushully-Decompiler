#pragma once
#include <sstream>
#include <unordered_set>
#include <variant>
#include <filesystem>
#include <fstream>
#include <span>
#include <array>

#include "types.h"

#define CP_UTF8 65001
#define CP_UTF16 1200
#define CP_932 932
std::wstring cp_to_utf16(u32 code_page, const std::string& input);
std::string utf16_to_cp(u32 code_page, const std::wstring& input);

struct BinaryHeader {
    char signature[8];

    u32 local_integer_1;
    u32 local_floats;
    u32 local_strings_1;
    u32 local_integer_2;
    u32 unknown_data;
    u32 local_strings_2;

    u32 sub_header_length; // Can this be anything other than 0x1C (size of the 6 integers below)?

    u32 table_1_length;
    u32 table_1_offset;

    u32 table_2_length;
    u32 table_2_offset;

    u32 table_3_length;
    u32 table_3_offset;
};
static_assert(sizeof(BinaryHeader) == 0x3C);

struct Header {
    Header(std::istream& fd) {
        std::array<char, 4> sig{};
        fd.read(sig.data(), sig.size());

        if (!std::memcmp(sig.data(), "SYS4", sig.size())) {
            fd.seekg(0, 0);
            fd.read((char*)&m_header, sizeof(BinaryHeader));
            m_length = 0x3C;
            m_is_ver5 = false;
        } else {
            std::wstring sys5{L"SYS5501 "};
            if (!std::memcmp(sig.data(), sys5.data(), sig.size())) {
                fd.seekg(0, 0);
                auto utf8_sig{utf16_to_cp(CP_UTF8, sys5)};
                std::memcpy(&m_header.signature, utf8_sig.data(), sizeof(m_header.signature));
                fd.seekg(16, 0);
                fd.read((char*)&m_header.local_integer_1, sizeof(BinaryHeader) - sizeof(m_header.signature));
                m_length = 0x44;
                m_is_ver5 = true;
            } else {
                fprintf(stderr, "Could not determine header version!\n");
                exit(-1);
            }
        }
    }

    Header(BinaryHeader&& binary) {
        m_header = std::move(binary);
        if (m_header.signature[3] == '5') {
            m_is_ver5 = true;
            m_length = 0x44;
        }
        else if (m_header.signature[3] == '4') {
            m_length = 0x3C;
        }
        else {
            fprintf(stderr, "Could not determine header version!\n");
            exit(-1);
        }
    }

    u32 GetLength() const {
        return m_length;
    }

    bool IsVer5() const {
        return m_is_ver5;
    }

    BinaryHeader& GetHeader() {
        return m_header;
    }

private:
    u32 m_length{};
    bool m_is_ver5{};
    BinaryHeader m_header;
};

struct Data_Array {
    u32 length;
    std::vector<u32> data;

    friend std::istream& operator>>(std::istream& is, Data_Array& da) {
        is.read((char*)&da.length, sizeof(da.length));
        da.data.reserve(da.length);
        for (u32 i{ 0 }; i < da.data.capacity(); ++i) {
            u32 val;
            is.read((char*)&val, sizeof(val));
            da.data.push_back(val);
        }
        return is;
    };
};

struct Argument {
    u32 type{};
    u32 raw_data{};
    std::string decoded_stringv4{};
    std::wstring decoded_stringv5{};
    Data_Array data_array{};

    friend std::istream& operator>>(std::istream& is, const Argument& arg) {
        is.read((char*)&arg.type, sizeof(arg.type));
        is.read((char*)&arg.raw_data, sizeof(arg.raw_data));
        return is;
    };
};

struct Instruction_Definition {
    const u32 op_code;
    const std::string_view label;
    const u32 argument_count;
};

struct Instruction {
    const Instruction_Definition* definition;
    std::vector<Argument> arguments;
    std::streamoff offset;

    Instruction(const Instruction_Definition* def, std::vector<Argument> args, std::streamoff off) :
        definition(def),
        arguments(std::move(args)),
        offset(off) {
    }
    Instruction(const Instruction_Definition* def, std::streamoff off) :
        definition(def),
        offset(off) {
        arguments.reserve(4);
    }
    
    Argument* GetArgument(size_t idx) {
        return &arguments.at(idx);
    }
};

const Instruction_Definition* instruction_for_op_code(u32 op_code, std::streamoff offset);
const Instruction_Definition* instruction_for_label(const std::string_view label);

inline constexpr bool is_control_flow(const Instruction_Definition* instruction) {
    return instruction->op_code == 0x8C ||
        instruction->op_code == 0x8F ||
        instruction->op_code == 0xA0 ||
        // code callbacks (UI buttons)
        instruction->op_code == 0xCC ||
        instruction->op_code == 0xFB ||
        // some call-looping thing
        instruction->op_code == 0xD4 ||
        instruction->op_code == 0x90 ||
        instruction->op_code == 0x7B;
}

inline constexpr bool is_control_flow(const Instruction& instruction) {
    return is_control_flow(instruction.definition);
}

inline constexpr bool is_array(const Instruction_Definition* instruction) {
    return instruction->op_code == 0x64;
}

inline constexpr bool is_label_argument(const Instruction& instruction, s32 x) {
    return ((instruction.definition->op_code == 0x8C || instruction.definition->op_code == 0x8F) && instruction.arguments[x].raw_data != 0xFFFFFFFF) ||
        (instruction.definition->op_code == 0xA0 && x > 0 && instruction.arguments[x].raw_data != 0xFFFFFFFF) ||
        ((instruction.definition->op_code == 0xCC || instruction.definition->op_code == 0xFB) && x > 0 && instruction.arguments[x].raw_data != 0xFFFFFFFF) ||
        (instruction.definition->op_code == 0xD4 && x >= 2 && instruction.arguments[x].raw_data != 0xFFFFFFFF) ||
        (instruction.definition->op_code == 0x90 && x >= 4 && instruction.arguments[x].raw_data != 0xFFFFFFFF) ||
        (instruction.definition->op_code == 0x7B && instruction.arguments[x].raw_data != 0xFFFFFFFF);
}

