// Coded by Kellindil

#include <iostream>
#include <ostream>
#include <sstream>
#include <set>
#include <vector>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "disassembler.h"
#include "age-shared.h"

int min2(int a, int b) {
	if (a < b) {
		return a;
	}
	return b;
}

int min2(int a, int b, int c) {
	return min2(min2(a, b), c);
}

Instruction* parse_instruction(int fd, Instruction_Definition* def, unsigned int offset, int* data_array_end) {
	std::vector<Argument*> arguments;

	unsigned int current = 0;
	while (current < def->argument_count) {
		Argument* arg = new Argument;
		// "8" as in "two unsigned ints"

		_read(fd, arg, 8);

		// If this instruction is a 'String' or copy-array argument, we have to alter data_array_end accordingly.
		if (arg->type == 2) {
			// Strings are all located at the end of the data array, XORed with 0xFF, and separated by 0xFF.
			unsigned int string_offset = HEADER_LENGTH + (arg->raw_data << 2);
			*data_array_end = min2(*data_array_end, string_offset);

			std::stringstream decodedstream;
			// mark current
			off_t cur_off = _lseek(fd, 0, SEEK_CUR);
			
			// jump to string offset
			_lseek(fd, string_offset, SEEK_SET);
			unsigned char character;
			_read(fd, &character, sizeof(character));
			while (character != 0xFF) {
				character = character ^ 0xFF;
				decodedstream << character;
				_read(fd, &character, sizeof(character));
			}

			std::string decoded = decodedstream.str();
			arg->decoded_string = sjis2utf8(decoded);

			// Go back to the instruction position
			_lseek(fd, cur_off, SEEK_SET);
		}
		else if (def->op_code == 0x64 && current == 1) {
			// This instruction actually references an array in the file's footer.
			unsigned int array_offset = HEADER_LENGTH + (arg->raw_data << 2);
			*data_array_end = min2(*data_array_end, array_offset);
			// mark current
			off_t cur_off = _lseek(fd, 0, SEEK_CUR);
			// Jump to array offset
			_lseek(fd, array_offset, SEEK_SET);

			unsigned int array_length;
			_read(fd, &array_length, sizeof(array_length));

			Data_Array data_array = { array_length, new unsigned int[array_length * sizeof(unsigned int)] };
			arg->data_array = data_array;
			_read(fd, arg->data_array.data, array_length * sizeof(unsigned int));

			// Go back to the instruction position
			_lseek(fd, cur_off, SEEK_SET);
		}

		//if (arg->type < 0 || arg->type > 14) {
		if (arg->type < 0 || (arg->type > 0xE && arg->type < 0x8003) || arg->type > 0x800B) {
			off_t cur_off = _lseek(fd, 0, SEEK_CUR);
			fprintf(stderr, "Pos : %x -> Opcode : %x, argument %d\n", cur_off, def->op_code, current);
			fprintf(stderr, "Unknown type : %x\n", arg->type);
			fprintf(stderr, "Value : %x\n", arg->raw_data);
			_close(fd);
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
	// Drive out the "\x3" suffix off of signature.
	// This might be because of the way I am reading the signature off of the binary; for now, truncate.
	std::basic_string<unsigned char> sig = header.signature;
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

std::string get_type_label(unsigned int type) {
	switch (type) {
		case 0:
			return "";
		case 1:
			// Not the best way to handle floats, but will do for now.
			return "float";
		case 2:
			return "";
		case 3:
			return "global-int";
		case 4:
			return "global-float";
		case 5:
			return "global-string";
		case 6:
			return "global-ptr";
		case 8:
			return "global-string-ptr";
		case 9:
			return "local-int";
		case 0xA:
			return "local-float";
		case 0xB:
			return "local-string";
		case 0xC:
			return "local-ptr";
		case 0xD:
			return "local-float-ptr";
		case 0xE:
			return "local-string-ptr";
		// In Sankai no Yubiwa
		// Why?
		case 0x8003:
			return "0x8003";
		case 0x8005:
			return "0x8005";
		case 0x8009:
			return "0x8009";
		case 0x800B:
			return "0x800B";
		default:
			fprintf(stderr, "Unknown type value: %x\n", type);
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

	int x = 0;
	for (auto& argument : instruction->arguments) {
		std::string type_label = get_type_label(argument->type);
		if (type_label != "") {
			stream << '(';
			stream << type_label;
			stream << ' ';
			stream << std::hex << argument->raw_data;
			stream << ')';
		}
		else if (argument->type == 2) {
			stream << '"';
			stream << argument->decoded_string;
			stream << '"';
		}
		else if (instruction->definition->op_code == 0x64 && argument->type == 0) {
			stream << "[";
			for (unsigned int i = 0; i < argument->data_array.length; i++) {
				stream << std::hex << argument->data_array.data[i];
				if (i < argument->data_array.length - 1) {
					stream << ' ';
				}
			}
			stream << "]";
		}
		else if (is_control_flow(instruction)) {
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

void write_script_file(std::filesystem::path& file_path, BinaryHeader header, std::vector<Instruction*> instructions) {
	// Find out which of our instructions are labels
	std::set<unsigned int> labels;
	for (auto& instruction : instructions) {
		// control flow instructions are call (0x8F), jmp (0x8C) and jcc (0xA0)
		// also code callback locations (button onClick e.g): 0xCC, 0xFB 
		if (is_control_flow(instruction)) {
			int x = 0;
			for (auto& argument : instruction->arguments) {
				if (is_label_argument(instruction, x)) {
					labels.insert(argument->raw_data);
				}
				x++;
			}
		}
	}

	int fd = open_or_die(file_path, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, S_IREAD | S_IWRITE);

	std::string header_value = disassemble_header(header);
	_write(fd, header_value.c_str(), header_value.length());

	for (std::vector<Instruction*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {
		// If this instruction is referenced as a label, make it clear
		if (labels.find((*iterator)->offset) != labels.end()) {
			std::stringstream label_instr;
			label_instr << "\nlabel_";
			label_instr << std::hex << ((*iterator)->offset << 2) + HEADER_LENGTH;
			label_instr << '\n';
			_write(fd, label_instr.str().c_str(), label_instr.str().length());
		}

		std::string instruction_value = disassemble_instruction(*iterator);
		_write(fd, instruction_value.c_str(), instruction_value.length());
	}

	_close(fd);
}

int disassemble(std::filesystem::path input, std::filesystem::path output) {
	fprintf(stdout, "Disassembling %s into %s\n", input.string().c_str(), output.string().c_str());

	int fd = open_or_die(input, O_RDONLY | O_BINARY);

	BinaryHeader header;
	_read(fd, &header, sizeof(header));
	
	int data_array_end = HEADER_LENGTH + (min2(header.table_1_offset, header.table_2_offset, header.table_3_offset) << 2);
	int strings_end = data_array_end;

	std::vector<Instruction*> instructions;
	off_t offset = _lseek(fd, 0, SEEK_CUR);
	//fprintf(stderr, "offset 0x%x data end 0x%x\n", offset, data_array_end);
	while (offset < data_array_end) {
		unsigned int op_code;
		_read(fd, &op_code, sizeof(op_code));

		Instruction_Definition* def = instruction_for_op_code(op_code);
		if (def == NULL) {
			fprintf(stderr, "Unknown instruction : 0x%x at 0x%x\n", op_code, _tell(fd)-0x4);
			_close(fd);
			exit(-1);
		}
		
		instructions.push_back(parse_instruction(fd, def, (offset - HEADER_LENGTH) >> 2, &data_array_end));
		offset = _lseek(fd, 0, SEEK_CUR);
	}

	_close(fd);

	// Find a portable and lightweight way to convert to unicode.
	// All of the Strings we read from these files are in CP932/CP942.
	write_script_file(output, header, instructions);

	return 0;
}

