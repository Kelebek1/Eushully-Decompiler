#include "age-shared.h"
#include "disassembler.h"

Instruction parse_instruction(std::istream& fd, const Instruction_Definition* def, std::streamoff offset, std::streamoff* data_array_end) {
    std::vector<Argument> arguments;
    arguments.reserve(def->argument_count);

    ;
    for (u32 current{0}; current < def->argument_count; ++current) {
        Argument& arg = arguments.emplace_back();
        // reads only 2 uint32_t
        fd >> arg;

        // If this instruction is a 'String' or copy-array argument, we have to alter data_array_end accordingly.
        if (arg.type == 2) {
            // Strings are all located at the end of the data array, XORed with 0xFF, and separated by 0xFF.
            std::streamoff string_offset = HEADER_LENGTH + (static_cast<uint64_t>(arg.raw_data) << 2);
            *data_array_end = std::min(*data_array_end, string_offset);

            // mark current
            std::streamoff cur_off = fd.tellg();

            // jump to string offset
            fd.seekg(string_offset, std::ios::beg);

            // read the string, XOR'ing each character
            std::string decoded;
            decoded.reserve(32);
            u8 character{};
            fd.read((char*)&character, sizeof(character));
            while (character != 0xFF) {
                character ^= 0xFF;
                decoded += character;
                fd.read((char*)&character, sizeof(character));
            }

            // convert it over to UTF8 for easier text editing
            arg.decoded_string = utf16_to_cp(CP_UTF8, cp_to_utf16(CP_932, decoded));

            // Go back to the instruction position
            fd.seekg(cur_off, std::ios::beg);
        } else if (def->op_code == 0x64 && current == 1) {
            // This instruction actually references an array in the file's footer.
            std::streamoff array_offset = HEADER_LENGTH + (static_cast<std::int64_t>(arg.raw_data) << 2);
            *data_array_end = std::min(*data_array_end, array_offset);

            // mark current
            std::streamoff cur_off = fd.tellg();

            // Jump to array offset
            fd.seekg(array_offset, std::ios::beg);

            fd >> arg.data_array;

            // Go back to the instruction position
            fd.seekg(cur_off, std::ios::beg);
        }

        if (arg.type < 0 || (arg.type > 0xE && arg.type < 0x8003) || arg.type > 0x800B) {
            std::streamoff cur_off = fd.tellg();
            fprintf(stderr, "Pos : %llx -> Opcode : %x, argument %d\n", cur_off, def->op_code, current);
            fprintf(stderr, "Unknown type : %x\n", arg.type);
            fprintf(stderr, "Value : %x\n", arg.raw_data);
            exit(-1);
        }
    }

    return Instruction(def, arguments, offset);
}

std::string disassemble_header(BinaryHeader& header) {
    static constexpr std::string_view HEADER_PREFIX = "==Binary Information - do not edit==\n";
    static constexpr std::string_view HEADER_SUFFIX = "====\n\n";
    static constexpr std::string_view SIGNATURE_PREFIX = "signature = ";
    static constexpr std::string_view VARS_PREFIX = "\nlocal_vars = { ";
    static constexpr std::string_view VARS_SUFFIX = " }\n";
    std::stringstream stream(std::stringstream::out);

    stream << HEADER_PREFIX;
    stream << SIGNATURE_PREFIX;
    stream.write((char*)header.signature, sizeof(header.signature));

    stream << VARS_PREFIX;
    stream << std::hex << header.local_integer_1;
    stream << ' ';
    stream << std::hex << header.local_floats;
    stream << ' ';
    stream << std::hex << header.local_strings_1;
    stream << ' ';
    stream << std::hex << header.local_integer_2;
    stream << ' ';
    stream << std::hex << header.unknown_data;
    stream << ' ';
    stream << std::hex << header.local_strings_2;
    stream << VARS_SUFFIX;

    stream << HEADER_SUFFIX;
    stream.flush();

    return stream.str();
}

const std::string get_type_label(u32 type) {
    switch (type) {
    case 0:   return "";
        // Not the best way to handle floats, but will do for now.
    case 1:   return "float";
    case 2:   return "";
    case 3:   return "global-int";
    case 4:   return "global-float";
    case 5:   return "global-string";
    case 6:   return "global-ptr";
    case 8:   return "global-string-ptr";
    case 9:   return "local-int";
    case 0xA: return "local-float";
    case 0xB: return "local-string";
    case 0xC: return "local-ptr";
    case 0xD: return "local-float-ptr";
    case 0xE: return "local-string-ptr";
        // In Sankai no Yubiwa onwards, why?
        // Another type of int and string?
    case 0x8003: return "0x8003";
    case 0x8005: return "0x8005";
    case 0x8009: return "0x8009";
    case 0x800B: return "0x800B";

    default: {
        fprintf(stderr, "Unknown type value: %x\n", type);
        exit(-1);
    }
    }
}

std::string disassemble_instruction(const Instruction& instruction) {
    std::stringstream stream(std::stringstream::out);

    // Give the loc of where we are
    //----------------
    // stream << std::hex << (instruction->offset << 2) + HEADER_LENGTH << " (" << std::hex << (instruction->offset << 2) << ":" << std::hex << instruction->offset << "): ";
    //----------------

    stream << instruction.definition->label;
    if (instruction.arguments.size() > 0) {
        stream << ' ';
    }

    s32 x = 0;
    for (const auto& argument : instruction.arguments) {
        std::string type_label = get_type_label(argument.type);
        if (type_label != "") {
            // e.g. (global_int 17A)
            stream << '(';
            stream << type_label << ' ' << std::hex << argument.raw_data;
            stream << ')';
        } else if (argument.type == 2) {
            // e.g. "this is a string"
            stream << '"';
            stream << argument.decoded_string;
            stream << '"';
        } else if (instruction.definition->op_code == 0x64 && argument.type == 0) {
            // e.g. [1 2 3 4 5 6]
            stream << "[";
            for (u32 i = 0; i < argument.data_array.length; i++) {
                stream << std::hex << argument.data_array.data[i];
                if (i < argument.data_array.length - 1) {
                    stream << ' ';
                }
            }
            stream << "]";
        } else if (is_control_flow(instruction)) {
            // e.g. label_99C8
            if (is_label_argument(instruction, x)) {
                stream << "label_";
                stream << std::right << std::setfill('0') << std::setw(8) << 
                    std::hex << HEADER_LENGTH + (static_cast<uint64_t>(argument.raw_data) << 2);
            } else {
                stream << std::hex << argument.raw_data;
            }
        } else {
            stream << std::hex << argument.raw_data;
        }
        if (x < instruction.arguments.size() - 1) {
            stream << ' ';
        }
        x++;
    }

    stream << '\n';

    stream.flush();
    return stream.str();
}

std::stringstream write_script_file(BinaryHeader& header, std::span<Instruction> instructions) {
    // Find out which of our instructions are labels
    std::unordered_set<u32> labels;
    for (auto& instruction : instructions) {
        if (is_control_flow(instruction)) {
            s32 x{0};
            for (auto& argument : instruction.arguments) {
                if (is_label_argument(instruction, x)) {
                    labels.insert(argument.raw_data);
                }
                x++;
            }
        }
    }

    std::stringstream output(std::stringstream::in | std::stringstream::out | std::stringstream::binary);

    output << disassemble_header(header);

    for (auto& instruction : instructions) {
        // If this instruction is referenced as a label, make it clear
        if (labels.find(instruction.offset) != labels.end()) {
            std::stringstream label_instr;
            label_instr << "\nlabel_";
            label_instr << std::right << std::setfill('0') << std::setw(8) << 
                std::hex << HEADER_LENGTH + (instruction.offset << 2);
            label_instr << '\n';
            output << std::move(label_instr.str());
        }

        output << disassemble_instruction(instruction);
    }

    output.flush();
    return output;
}

std::stringstream disassemble(std::istream& fd) {
    BinaryHeader header;
    fd >> header;

    std::streamoff data_array_end = HEADER_LENGTH + (static_cast<uint64_t>(std::min(std::min(header.table_1_offset, header.table_2_offset), header.table_3_offset)) << 2);
    std::streamoff strings_end = data_array_end;

    std::vector<Instruction> instructions;
    instructions.reserve(5'000);

    while (fd.tellg() < data_array_end) {
        std::streamoff offset = fd.tellg();
        u32 op_code{};
        fd.read((char*)&op_code, sizeof(op_code));

        const Instruction_Definition* def = instruction_for_op_code(op_code, offset);
        instructions.emplace_back(parse_instruction(fd, def, (offset - HEADER_LENGTH) >> 2, &data_array_end));
    }

    return write_script_file(header, instructions);
}