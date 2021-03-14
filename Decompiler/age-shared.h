#pragma once
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <map>
#include <unordered_map>
#include <filesystem>
#include "types.h"

static constexpr size_t HEADER_LENGTH = 60;
static const std::string HEADER_PREFIX = "==Binary Information - do not edit==\n";
static const std::string HEADER_SUFFIX = "====\n\n";
static const std::string SIGNATURE_PREFIX = "signature = ";
static const std::string VARS_PREFIX = "\nlocal_vars = { ";
static const std::string VARS_SUFFIX = " }\n";

#define CP_UTF8 65001
#define CP_932 932

std::wstring cp_to_utf16(u32 code_page, const std::string& input);
std::string utf16_to_cp(u32 code_page, const std::wstring& input);

struct BinaryHeader {
    u8 signature[8];

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

    friend std::istream& operator>>(std::istream& is, BinaryHeader& hdr) {
        is.read((char*)&hdr, sizeof(BinaryHeader));
        return is;
    };
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
    std::string decoded_string{};
    Data_Array data_array{};

    friend std::istream& operator>>(std::istream& is, const Argument& arg) {
        is.read((char*)&arg.type, sizeof(arg.type));
        is.read((char*)&arg.raw_data, sizeof(arg.raw_data));
        return is;
    };
};

struct Instruction_Definition {
    u32 op_code;
    std::string_view label;
    u32 argument_count;
};

struct Instruction {
    const Instruction_Definition* definition;
    std::vector<const Argument*> arguments;
    std::streamoff offset;

    Instruction(const Instruction_Definition* def, std::vector<const Argument*> args, std::streamoff off) :
        definition(std::move(def)),
        arguments(std::move(args)),
        offset(std::move(off)) {

    }
};

const Instruction_Definition* instruction_for_op_code(u32 op_code, std::streamoff offset);
const Instruction_Definition* instruction_for_label(const std::string& label);

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

inline constexpr bool is_control_flow(const Instruction* instruction) {
    return is_control_flow(instruction->definition);
}

inline constexpr bool is_array(const Instruction_Definition* instruction) {
    return instruction->op_code == 0x64;
}

inline constexpr bool is_label_argument(const Instruction* instruction, s32 x) {
    return ((instruction->definition->op_code == 0x8C || instruction->definition->op_code == 0x8F) && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
        (instruction->definition->op_code == 0xA0 && x > 0 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
        ((instruction->definition->op_code == 0xCC || instruction->definition->op_code == 0xFB) && x > 0 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
        (instruction->definition->op_code == 0xD4 && x >= 2 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
        (instruction->definition->op_code == 0x90 && x >= 4 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
        (instruction->definition->op_code == 0x7B && instruction->arguments[x]->raw_data != 0xFFFFFFFF);
}

