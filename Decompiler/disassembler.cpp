#include <set>
#include <sstream>

#include "disassembler.h"
#include "age-shared.h"

Instruction* parse_instruction(std::ifstream& fd, const Instruction_Definition* def, std::streamoff offset, std::streamoff* data_array_end) {
    std::vector<const Argument*> arguments;

    u32 current = 0;
    while (current < def->argument_count) {
        Argument* arg = new Argument;
        // reads only 2 uint32_t
        fd >> *arg;

        // If this instruction is a 'String' or copy-array argument, we have to alter data_array_end accordingly.
        if (arg->type == 2) {
            // Strings are all located at the end of the data array, XORed with 0xFF, and separated by 0xFF.
            std::streamoff string_offset = HEADER_LENGTH + static_cast<std::int64_t>(arg->raw_data << 2);
            *data_array_end = std::min(*data_array_end, string_offset);
            
            // mark current
            std::streamoff cur_off = fd.tellg();

            // jump to string offset
            fd.seekg(string_offset, std::ios::beg);

            // read the string, XOR'ing each character
            std::stringstream decodedstream;
            u8 character; 
            fd.read((char*)&character, sizeof(character));
            while (character != 0xFF) {
                character ^= 0xFF;
                decodedstream << character;
                fd.read((char*)&character, sizeof(character));
            }
            std::string decoded = decodedstream.str();

            // convert it over to UTF8 for easier text editing
            arg->decoded_string = cp932_to_utf8(decoded);

            // Go back to the instruction position
            fd.seekg(cur_off, std::ios::beg);
        }
        else if (def->op_code == 0x64 && current == 1) {
            // This instruction actually references an array in the file's footer.
            std::streamoff array_offset = HEADER_LENGTH + static_cast<std::int64_t>(arg->raw_data << 2);
            *data_array_end = std::min(*data_array_end, array_offset);

            // mark current
            std::streamoff cur_off = fd.tellg();

            // Jump to array offset
            fd.seekg(array_offset, std::ios::beg);

            fd >> arg->data_array;

            // Go back to the instruction position
            fd.seekg(cur_off, std::ios::beg);
        }

        if (arg->type < 0 || (arg->type > 0xE && arg->type < 0x8003) || arg->type > 0x800B) {
            std::streamoff cur_off = fd.tellg();
            fprintf(stderr, "Pos : %llx -> Opcode : %x, argument %d\n", cur_off, def->op_code, current);
            fprintf(stderr, "Unknown type : %x\n", arg->type);
            fprintf(stderr, "Value : %x\n", arg->raw_data);
            fd.close();
            exit(-1);
        }
        
        arguments.push_back(arg);
        current++;
    }

    Instruction* instr = new Instruction(def, arguments, offset);

    return instr;
}

std::string disassemble_header(BinaryHeader header) {
    std::stringstream stream;
    stream << HEADER_PREFIX;
    
    stream << SIGNATURE_PREFIX;
    // Drive out the erroneous last byte of signature.
    std::basic_string<u8> sig{ header.signature };
    stream << sig.substr(0, sizeof(header.signature)).c_str();

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
        case 0x8003: return "0x8003";
        case 0x8005: return "0x8005";
        case 0x8009: return "0x8009";
        case 0x800B: return "0x800B";
            
        default: fprintf(stderr, "Unknown type value: %x\n", type);
    }
}

std::string disassemble_instruction(Instruction* instruction) {
    std::stringstream stream;

    // Give the loc of where we are
    //----------------
    // stream << std::hex << (instruction->offset << 2) + HEADER_LENGTH << " (" << std::hex << (instruction->offset << 2) << ":" << std::hex << instruction->offset << "): ";
    //----------------

    stream << instruction->definition->label;
    if (instruction->arguments.size() > 0)
        stream << ' ';

    s32 x = 0;
    for (auto& argument : instruction->arguments) {
        std::string type_label = get_type_label(argument->type);
        if (type_label != "") {
            // e.g. (global_int 17A)
            stream << '(';
            stream << type_label << ' ' << std::hex << argument->raw_data;
            stream << ')';
        }
        else if (argument->type == 2) {
            // e.g. "this is a string"
            stream << '"';
            stream << argument->decoded_string;
            stream << '"';
        }
        else if (instruction->definition->op_code == 0x64 && argument->type == 0) {
            // e.g. [1 2 3 4 5 6]
            stream << "[";
            for (u32 i = 0; i < argument->data_array.length; i++) {
                stream << std::hex << argument->data_array.data[i];
                if (i < argument->data_array.length - 1) {
                    stream << ' ';
                }
            }
            stream << "]";
        }
        else if (is_control_flow(instruction)) {
            // e.g. label_99C8
            if (is_label_argument(instruction, x)) {
                stream << "label_";
                stream << std::hex << (argument->raw_data << 2) + HEADER_LENGTH;
            } else {
                stream << std::hex << argument->raw_data;
            }
        }
        else {
            stream << std::hex << argument->raw_data;
        }
        if (x < instruction->arguments.size() - 1) {
            stream << ' ';
        }
        x++;
    }

    stream << '\n';

    stream.flush();
    return stream.str();
}

void write_script_file(std::filesystem::path& file_path, BinaryHeader& header, std::vector<Instruction*>& instructions) {
    // Find out which of our instructions are labels
    std::set<u32> labels;
    for (auto& instruction : instructions) {
        // control flow instructions are call (0x8F), jmp (0x8C) and jcc (0xA0)
        // also code callback locations (e.g. button onClick): 0xCC, 0xFB 
        if (is_control_flow(instruction)) {
            s32 x{0};
            for (auto& argument : instruction->arguments) {
                if (is_label_argument(instruction, x)) {
                    labels.insert(argument->raw_data);
                }
                x++;
            }
        }
    }

    std::ofstream output(file_path, std::ios::out);

    output << disassemble_header(header);

    for (auto& instruction : instructions) {
        // If this instruction is referenced as a label, make it clear
        if (labels.find(instruction->offset) != labels.end()) {
            std::stringstream label_instr;
            label_instr << "\nlabel_";
            label_instr << std::hex << (instruction->offset << 2) + HEADER_LENGTH;
            label_instr << '\n';
            output << label_instr.str();
        }

        output << disassemble_instruction(instruction);
    }

    output.close();
}

s32 disassemble(std::filesystem::path input, std::filesystem::path output) {
    fprintf(stdout, "Disassembling %s into %s\n", input.string().c_str(), output.string().c_str());

    std::ifstream fd(input, std::ios::in | std::ios::binary);
    BinaryHeader header;
    fd >> header;

    std::streamoff data_array_end = HEADER_LENGTH + (std::min(std::min(header.table_1_offset, header.table_2_offset), header.table_3_offset) << 2);
    std::streamoff strings_end = data_array_end;

    std::vector<Instruction*> instructions;
    while (fd.tellg() < data_array_end) {
        std::streamoff offset = fd.tellg();
        u32 op_code; 
        fd.read((char*)&op_code, sizeof(op_code));

        const Instruction_Definition* def = instruction_for_op_code(op_code);
        if (def == nullptr) {
            fprintf(stderr, "Unknown instruction : 0x%x at 0x%llx\n", op_code, offset);
            fd.close();
            exit(-1);
        }

        instructions.push_back(parse_instruction(fd, def, (offset - HEADER_LENGTH) >> 2, &data_array_end));
    }

    fd.close();

    write_script_file(output, header, instructions);

    return 0;
}