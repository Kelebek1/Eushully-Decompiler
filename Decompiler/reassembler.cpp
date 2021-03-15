#include "age-shared.h"
#include "reassembler.h"

/*
template<typename T>
inline void update_max(std::atomic<T>& atom, const T val) {
    for (T atom_val = atom;
         atom_val < val &&
         !atom.compare_exchange_strong(atom_val, val, std::memory_order_seq_cst);
         );
}
static std::atomic<size_t> max_val;
static std::vector<size_t> average;

    update_max(max_val, array_arguments.size());
    average.push_back(array_arguments.size());
    size_t avg = std::accumulate(average.begin(), average.end(), 0) / average.size();
    std::cout << "array_arguments " << max_val << " " << "curr avg: " << avg << '\n';
*/

static const std::regex re_parse_instr("(^[\\w\\-_]+) ?|(label_[0-9a-fA-F]+)");
static const std::regex re_parse_args("\\((\\w+?\\-?\\w+?\\-?\\w+?) ([0-9a-fA-F]+)\\)|(\\\".*?\\\")|label_([0-9a-fA-F]+)|\\[(.+?)\\]|([0-9a-fA-F]+)");
static const std::regex re_local_vars("([0-9a-fA-F]+?) ");

enum ARGUMENT_TYPE {
    REG_TYPE = 0,
    REG_NUM,
    STR,
    LABEL,
    ARRAY,
    VALUE,
    TOTAL
};

u32 get_type(const std::string name) {
    // most frequent, by far, is local-int
    if (name == "local-int")          return 9;
    else if (name == "local-ptr")          return 0xC;
    else if (name == "global-int")         return 3;
    else if (name == "global-float")       return 4;
    else if (name == "global-string")      return 5;
    else if (name == "global-ptr")         return 6;
    else if (name == "global-string-ptr")  return 8;
    else if (name == "local-float")        return 0xA;
    else if (name == "local-string")       return 0xB;
    else if (name == "local-string-ptr")   return 0xE;
    else if (name == "float")              return 1;
    else if (name == "local-float-ptr")    return 0xD;
    // Only seen in arterial so far, with value 0. What could this be?
    else if (name == "unknown0x8003")      return 0x8003;
    // In Sankai no Yubiwa
    else if (name == "unknown0x8005")      return 0x8005;
    else if (name == "unknown0x8009")      return 0x8009;
    else if (name == "unknown0x800B")      return 0x800B;

    fprintf(stdout, "Unknown variable type: %s\n", name.c_str());
    exit(-2);
}

auto parse_multiple_arguments(std::string line, const std::regex& regex) {
    std::vector<std::vector<std::string>> elements;
    elements.reserve(ARGUMENT_TYPE::TOTAL);
    std::match_results<std::string::const_iterator> matches;

    while (std::regex_search(line, matches, regex)) {
        std::vector<std::string> sub_elements;
        sub_elements.reserve(matches.size() - 1);
        for (u32 i{1}; i < matches.size(); ++i) {
            sub_elements.push_back(std::move(matches.str(i)));
        }
        elements.push_back(std::move(sub_elements));
        line = std::move(matches.suffix().str());
    }

    return elements;
}

BinaryHeader parse_header(std::istream& fd) {
    // All we are interested in are the signature and local_vars
    BinaryHeader header;
    std::string line;

    std::getline(fd, line); // ==Binary Information - do not edit==

    std::getline(fd, line); // Signature = SYSxxxx
    std::memcpy(header.signature, line.substr(line.find("= ") + 2, line.length()).data(), sizeof(header.signature));

    std::getline(fd, line); // local_vars = { }
    const auto local_vars{ parse_multiple_arguments(line, re_local_vars) };

    if (local_vars.size() < 6) {
        fprintf(stdout, "Header is corrupted, there should be 6 local_vars, but could only read %d\n", (u32)local_vars.size());
        _exit(-1);
    }

    // Local vars are in hex string form, separated by a whitespace
    header.local_integer_1 = std::stoul(local_vars[0][0], nullptr, 16);
    header.local_floats = std::stoul(local_vars[1][0], nullptr, 16);
    header.local_strings_1 = std::stoul(local_vars[2][0], nullptr, 16);
    header.local_integer_2 = std::stoul(local_vars[3][0], nullptr, 16);
    header.unknown_data = std::stoul(local_vars[4][0], nullptr, 16);
    header.local_strings_2 = std::stoul(local_vars[5][0], nullptr, 16);

    std::getline(fd, line); // ====

    // We have now read all of our header, and positioned the file handle at the start of our instruction list
    return header;
}

inline constexpr size_t compute_length(Instruction_Definition definition) {
    // op_code (4 bytes) + 2 * (4 bytes) * (argument_count)
    return 4 + (static_cast<size_t>(definition.argument_count) << 3);
}

std::stringstream write_assembled_file(BinaryHeader& header, std::span<Instruction> instructions, std::string_view string_data, std::span<u32> footer_data) {
    std::stringstream output(std::stringstream::in | std::stringstream::out | std::stringstream::binary);

    output.write((char*)&header, sizeof(header));

    for (const auto& instruction : instructions) {
        output.write((char*)&instruction.definition->op_code, sizeof(u32));
        for (const auto& argument : instruction.arguments) {
            output.write((char*)&argument.type, sizeof(u32));
            output.write((char*)&argument.raw_data, sizeof(u32));
        }
    }

    output.write(string_data.data(), string_data.length());
    output.write((char*)footer_data.data(), footer_data.size() * sizeof(u32));
    output.flush();
    return output;
}

std::stringstream assemble(std::istream& fd) {
    BinaryHeader header = parse_header(fd);
    header.sub_header_length = 0x1C; // can this be anything else?
    // Note that the header is not fully initialized : some of its information may change and has to be computed again.
    // For now, we need to parse the instruction list.

    /*
     * This is using a lot of maps and sets since I am trying to do most of the job at once whilst reading the disassembled file.
     * This might be more readable if I iterated over the reassembled instructions to restore the changed information.
    */
    std::vector<Instruction> instructions;
    instructions.reserve(5'000);
    // We'll have to "remember" the offsets to the 'label' functions...
    std::unordered_map<u32, u32> label_to_offset;
    // ... in order to replace them in the arguments that reference them
    std::vector<std::pair<size_t, size_t>> label_arguments;
    label_arguments.reserve(2'000);
    // We also need to record the offsets of these instructions that are part of the sub-header : 0x71, 0x3 and 0x8f
    std::unordered_set<u32> instr_3_offsets;
    std::unordered_set<u32> instr_71_offsets;
    std::unordered_set<u32> instr_8f_offsets;
    // we'll have to replace the string arguments with their offset in the assembled file
    std::vector<std::pair<size_t, size_t>> string_arguments;
    string_arguments.reserve(200);
    // Finally, we'll have to replace the arrays with their offset in the footer of the assembled file
    std::vector<std::pair<size_t, size_t>> array_arguments;
    array_arguments.reserve(100);

    u32 data_array_end = HEADER_LENGTH;

    std::string line;
    u32 line_count = 6;
    while (!fd.eof()) {
        std::getline(fd, line);

        // Skip empty lines
        if (line == "") continue;
        // Skip lines starting with // for single-line comments
        else if (line.starts_with("//")) continue;
        // Skip multi-line comment, cut out everything up to the ending */
        else if (line.starts_with("/*")) {
            while (!fd.eof() && line.find("*/") == std::string::npos)
                std::getline(fd, line);
            if (fd.eof()) break;
            line = line.substr(line.find("*/") + 2, line.length());
        }

        const auto matches{parse_multiple_arguments(line, re_parse_instr)};
        if (matches.size() == 0) {
            fprintf(stderr, "Failed to parse line %d.\n", line_count);
            exit(-1);
        }
        std::string instruction{matches[0][0]};

        if (instruction.starts_with("label_")) {
            label_to_offset[std::stoul(instruction.substr(6), nullptr, 16)] = data_array_end;
            continue;
        }

        const Instruction_Definition* definition = instruction_for_label(instruction);

        Instruction& new_instruction = instructions.emplace_back(definition, data_array_end);

        if (definition->argument_count > 0) {
            auto str_arguments{ parse_multiple_arguments(line.substr(instruction.length() + 1, line.length()), re_parse_args) };

            if (definition->argument_count != str_arguments.size()) {
                fprintf(stderr, "Argument mismatch for %s on line %d.\n", instruction.c_str(), line_count);
                fprintf(stderr, "Expected %d args but found %d.\n", definition->argument_count, (u32)str_arguments.size());
                exit(-1);
            }

            // read in the arguments of this function
            for (auto& arg : str_arguments) {
                Argument& current = new_instruction.arguments.emplace_back();
                std::pair<size_t, size_t> current_index{instructions.size() - 1, new_instruction.arguments.size() - 1};

                // str_arguments is a 2D vector of argument and argument info
                // str_arguments[x][REG_TYPE] contains scope and type          (e.g. the "global-int" in "global-int 7")
                // str_arguments[x][REG_NUM]  contains value when above is set (e.g. the 7 in "global-int 7")
                // str_arguments[x][STR]      contains string literals
                // str_arguments[x][LABEL]    contains labels
                // str_arguments[x][VALUE]    contains number literals

                if (!arg[ARGUMENT_TYPE::REG_TYPE].empty()) {
                    current.type = get_type(arg[ARGUMENT_TYPE::REG_TYPE]);
                    current.raw_data = std::stoul(arg[ARGUMENT_TYPE::REG_NUM], nullptr, 16);
                } else if (!arg[ARGUMENT_TYPE::STR].empty()) {
                    // Strip the quotes
                    arg[ARGUMENT_TYPE::STR] = arg[ARGUMENT_TYPE::STR].substr(1, arg[ARGUMENT_TYPE::STR].length() - 2);

                    // We'll have to "restore" this argument's data later on as the offset where the string will be written
                    current.type = 2;

                    // Convert back to CP932
                    current.decoded_string = utf16_to_cp(CP_932, cp_to_utf16(CP_UTF8, arg[ARGUMENT_TYPE::STR]));

                    string_arguments.push_back(current_index);
                } else if (!arg[ARGUMENT_TYPE::LABEL].empty()) {
                    current.type = 0;
                    // We don't know -yet- the actual offset of this label
                    current.raw_data = std::stoul(arg[ARGUMENT_TYPE::LABEL], nullptr, 16);
                    label_arguments.push_back(current_index);
                } else if (!arg[ARGUMENT_TYPE::ARRAY].empty()) {
                    std::vector<u32> data;
                    data.reserve(4);
                    s32 start = 0, end = 0;
                    while (start < arg[ARRAY].length()) {
                        end = arg[ARGUMENT_TYPE::ARRAY].find_first_of(" ", start);
                        if (end < 0) end = arg[ARRAY].length();
                        data.push_back(std::stoul(arg[ARGUMENT_TYPE::ARRAY].substr(start, end), nullptr, 16));
                        start = end + 1;
                    }

                    // We'll have to "restore" this argument's data later on as the offset where the array will be written
                    current.type = 0;
                    current.data_array = {(u32)data.size(), data};
                    array_arguments.push_back(current_index);
                } else if (!arg[ARGUMENT_TYPE::VALUE].empty()) {
                    current.type = 0;
                    current.raw_data = std::stoul(arg[VALUE], nullptr, 16);
                } else {
                    fprintf(stderr, "Bad argument for %s on line %d.\n", instruction.c_str(), line_count);
                    _exit(-1);
                }
            }
        }

        if (definition->op_code == 0x3)        instr_3_offsets.insert(data_array_end);
        else if (definition->op_code == 0x71) instr_71_offsets.insert(data_array_end);
        else if (definition->op_code == 0x8F) instr_8f_offsets.insert(data_array_end);

        data_array_end += compute_length(*definition);
        line_count++;
    }

    // Before writing our instructions, we need to restore the label, string and array offsets
    for (auto& [instr_idx, arg_idx] : label_arguments) {
        Instruction* instr = &instructions[instr_idx];
        Argument* arg = instr->GetArgument(arg_idx);
        arg->raw_data = (label_to_offset[arg->raw_data] - HEADER_LENGTH) >> 2;
    }

    // Restore the strings offsets
    std::string string_data;
    string_data.reserve(5'000);
    u32 current_string_offset = data_array_end;
    for (auto& [instr_idx, arg_idx] : string_arguments) {
        Instruction* instr = &instructions[instr_idx];
        Argument* arg = instr->GetArgument(arg_idx);

        arg->raw_data = (current_string_offset - HEADER_LENGTH) >> 2;
        // we have at least one 0xFF as a separator, + as many as needed to reach a multiple of four for the next offset.
        current_string_offset += arg->decoded_string.length() + 1;

        for (u32 i = 0; i < arg->decoded_string.length(); i++) {
            string_data += arg->decoded_string[i] ^ 0xFF;
        }

        u32 padding = 4 - ((current_string_offset) % 4);
        for (u32 i = 0; i < padding + 1; i++) {
            string_data += 0xFF;
        }
        current_string_offset += padding;
    }

    // assemble the offset indexing of the footer
    std::vector<u32> footer_data;
    footer_data.reserve(1'000);
    // Restore the array offsets
    u32 current_array_offset = (current_string_offset - HEADER_LENGTH) >> 2;
    for (auto& [instr_idx, arg_idx] : array_arguments) {
        Instruction* instr = &instructions[instr_idx];
        Argument* arg = instr->GetArgument(arg_idx);

        arg->raw_data = current_array_offset;
        footer_data.push_back(arg->data_array.length);
        current_array_offset += arg->data_array.length + 1;
        footer_data.insert(footer_data.end(), 
                           arg->data_array.data.begin(), arg->data_array.data.end());
    }

    std::for_each(instr_71_offsets.begin(), instr_71_offsets.end(), 
                  [&](u32 offset) { footer_data.push_back((offset - HEADER_LENGTH) >> 2); });
    header.table_1_length = instr_71_offsets.size();
    header.table_1_offset = current_array_offset;

    std::for_each(instr_3_offsets.begin(), instr_3_offsets.end(),
                  [&](u32 offset) { footer_data.push_back((offset - HEADER_LENGTH) >> 2); });
    header.table_2_length = instr_3_offsets.size();
    header.table_2_offset = header.table_1_offset + header.table_1_length;

    std::for_each(instr_8f_offsets.begin(), instr_8f_offsets.end(),
                  [&](u32 offset) { footer_data.push_back((offset - HEADER_LENGTH) >> 2); });
    header.table_3_length = instr_8f_offsets.size();
    header.table_3_offset = header.table_2_offset + header.table_2_length;

    return write_assembled_file(header, instructions, string_data, footer_data);
}