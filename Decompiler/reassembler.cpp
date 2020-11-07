// Coded by Kellindil

#include <fcntl.h>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <filesystem>
#include <fstream>

#include "age-shared.h"

// Note that separator will be read and ignored. Current offset will be right after it at the end of this call.
// In addition to the given separator, we'll stop on EOF and line feed.
unsigned int read_hex_string(int fd, char separator) {
	std::string value;
	unsigned char character;
	_read(fd, &character, sizeof(character));
	while (character != separator && character != EOF && character != '\n') {
		value += character;
		_read(fd, &character, sizeof(character));
	}

	std::stringstream stream;
	stream << std::hex << value;

	unsigned int result;
	stream >> result;

	return result;
}

unsigned int get_type(const char* name) {
	// most frequent, by far, is local-int
	if (strcmp(name, "local-int") == 0) {
		return 9;
	} else if (strcmp(name, "local-ptr") == 0) {
		return 0xC;
	} else if (strcmp(name, "global-int") == 0) {
		return 3;
	} else if (strcmp(name, "global-float") == 0) {
		return 4;
	} else if (strcmp(name, "global-string") == 0) {
		return 5;
	} else if (strcmp(name, "global-ptr") == 0) {
		return 6;
	} else if (strcmp(name, "global-string-ptr") == 0) {
		return 8;
	} else if (strcmp(name, "local-float") == 0) {
		return 0xA;
	} else if (strcmp(name, "local-string") == 0) {
		return 0xB;
	} else if (strcmp(name, "local-string-ptr") == 0) {
		return 0xE;
	} else if (strcmp(name, "float") == 0) {
		return 1;
	} else if (strcmp(name, "local-float-ptr") == 0) {
		// Only seen in arterial so far, with value 0. What could this be?
		return 0xD;
	} else if (strcmp(name, "unknown0x8003") == 0) {
		// In Sankai no Yubiwa
		return 0x8003;
	} else if (strcmp(name, "unknown0x8005") == 0) {
		return 0x8005;
	} else if (strcmp(name, "unknown0x8009") == 0) {
		return 0x8009;
	} else if (strcmp(name, "unknown0x800B") == 0) {
		return 0x800B;
	}
	fprintf(stdout, "Unknown typed variable : %s\n", name);
	exit(-2);
}

std::string read_instruction_label(int fd) {
	std::string line;

	std::string value = "";
	unsigned char character;
	int read = _read(fd, &character, sizeof(character));
	while (read > 0 && character == '\n') {
		read = _read(fd, &character, sizeof(character));
	}

	// Skip lines if it begins with a forward slash, intended as a comment
	while (read > 0 && character == '/') {
		while (read > 0 && character != '\n') {
			read = _read(fd, &character, sizeof(character));
		}
		read = _read(fd, &character, sizeof(character));
	}

	while (read > 0 && character != ' ' && character != '\n') {
		value += character;
		read = _read(fd, &character, sizeof(character));
	}
	return value;
}

BinaryHeader parse_header(int fd) {
	// All we are interested in are the signature and local_vars
	BinaryHeader header;

	_lseek(fd, HEADER_PREFIX.length() + SIGNATURE_PREFIX.length(), SEEK_SET);
	_read(fd, &header.signature, sizeof(header.signature));
	_lseek(fd, VARS_PREFIX.length(), SEEK_CUR);

	// Local vars are in hex string form, separated by a whitespace
	header.local_integer_1 = read_hex_string(fd, ' ');
	header.local_floats = read_hex_string(fd, ' ');
	header.local_strings_1 = read_hex_string(fd, ' ');
	header.local_integer_2 = read_hex_string(fd, ' ');
	header.unknown_data = read_hex_string(fd, ' ');
	header.local_strings_2 = read_hex_string(fd, ' ');

	// read_hex_string eats a white space
	_lseek(fd, VARS_SUFFIX.length() + HEADER_SUFFIX.length() - 1, SEEK_CUR);

	// We have now read all of our header, and positioned the file handle at the start of our instruction list
	return header;
}

unsigned int compute_length(Instruction_Definition definition) {
	// op_code (4 bytes) + 2 * (4 bytes) * (argument_count)
	return 4 + (definition.argument_count << 3);
}

void write_assembled_file(std::filesystem::path file_path, BinaryHeader header, std::vector<Instruction*> instructions, std::basic_string<unsigned char> string_data, std::vector<unsigned int> footer_data) {
	int fd = open_or_die(file_path, O_CREAT | O_TRUNC | O_WRONLY | O_BINARY, S_IREAD | S_IWRITE);

	_write(fd, &header, sizeof(header));

	for (std::vector<Instruction*>::iterator iterator = instructions.begin(); iterator != instructions.end(); ++iterator) {
		_write(fd, &(*iterator)->definition->op_code, sizeof(unsigned int));
		for (std::vector<Argument*>::iterator arg = (*iterator)->arguments.begin(); arg != (*iterator)->arguments.end(); ++arg) {
			_write(fd, &(*arg)->type, sizeof(unsigned int));
			_write(fd, &(*arg)->raw_data, sizeof(unsigned int));
		}
	}

	for (unsigned int i = 0; i < string_data.length(); i++) {
		_write(fd, &string_data[i], sizeof(unsigned char));
	}

	for (std::vector<unsigned int>::iterator iterator = footer_data.begin(); iterator != footer_data.end(); ++iterator) {
		unsigned int i = *iterator;
		_write(fd, &i, sizeof(unsigned int));
	}

	_close(fd);
}

int assemble(std::filesystem::path input, std::filesystem::path output) {
	fprintf(stdout, "Assembling %s into %s\n", input.string().c_str(), output.string().c_str());

	int fd = open_or_die(input, O_RDONLY | O_BINARY);

	BinaryHeader header = parse_header(fd);
	header.sub_header_length = 0x1c; // can this be anything else?
	// Note that the header is not fully initialized : some of its information may change and has to be computed again.
	// For now, we need to parse the instruction list.

	/*
	 * This is using a lot of maps and sets since I am trying to do most of the job at once whilst reading the disassembled file.
	 * This might be more readable if I iterated over the reassembled instructions to restore the changed information.
	*/
	std::vector<Instruction*> instructions;
	// We'll have to "remember" the offsets to the 'label' functions...
	std::map<unsigned int, unsigned int> label_to_offset;
	// ... in order to replace them in the arguments that reference them
	std::vector<Argument*> label_arguments;
	// We also need to record the offsets of these instructions that are part of the sub-header : 0x71, 0x3 and 0x8f
	std::set<unsigned int> instr_3_offsets;
	std::set<unsigned int> instr_71_offsets;
	std::set<unsigned int> instr_8f_offsets;
	// we'll have to replace the string arguments with their offset in the assembled file
	std::vector<Argument*> string_arguments;
	// Finally, we'll have to replace the arrays with their offset in the footer of the assembled file
	std::vector<Argument*> array_arguments;

	int data_array_end = HEADER_LENGTH;
	
	std::string instruction_label = read_instruction_label(fd);
	while (instruction_label != "") {
		if (strcmp(instruction_label.substr(0, 6).c_str(), "label_") == 0) {
			std::stringstream stream;
			stream << std::hex << instruction_label.substr(6);
			unsigned int label_offset;
			stream >> label_offset;
			label_to_offset[label_offset] = data_array_end;
		} else {
			Instruction_Definition* definition = instruction_for_label(instruction_label);
			if (definition == NULL) {
				fprintf(stderr, "unknown instruction : %s\n", instruction_label.c_str());
				exit(-1);
			}

			// read in the arguments of this function
			std::vector<Argument*> arguments;
			for (unsigned int i = 0; i < definition->argument_count; i++) {
				Argument* current = new Argument;

				unsigned char character;
				_read(fd, &character, sizeof(character));
				if (character == '(') {
					// This is a typed argument. Read its full type
					std::string type_value = "";
					while (character != ' ') {
						_read(fd, &character, sizeof(character));
						type_value += character;
					}
					// cut out the trailing space
					current->type = get_type(type_value.substr(0, type_value.length() - 1).c_str());
					current->raw_data = read_hex_string(fd, ')');

					// Whatever it is, eat the next
					_lseek(fd, 1, SEEK_CUR);
				} else if (character == '"') {
					// This is a String. Read until the closing quote
					std::string ss;
					bool breakLoop = false;
					_read(fd, &character, sizeof(character));
					while (!breakLoop) {
						// Special case : empty string
						if (character == '"') {
							_read(fd, &character, sizeof(character));
							if ((definition->op_code != 0x6E && character == ' ') || character == '\n' || character == EOF) {
								breakLoop = true;
							} else {
								ss += '"';
							}
						} else {
							ss += character;
							_read(fd, &character, sizeof(character));
						}
					}

					// We'll have to "restore" this argument's data later on as the offset where the string will be written
					current->type = 2;

					// Convert back to SJIS
					current->decoded_string = utf82sjis(ss);

					string_arguments.push_back(current);
				} else if (character == 'l') {
					// eat away the "abel_" prefix to the actual data
					_lseek(fd, 5, SEEK_CUR);
					current->type = 0;
					// We don't know -yet- the actual offset of this label
					current->raw_data = read_hex_string(fd, ' ');
					label_arguments.push_back(current);
				} else if (character == '[') {
					std::vector<unsigned int> data;
					_read(fd, &character, sizeof(character));

					while (character != ']') {
						std::string hex_value;
						while (character != ']' && character != ' ') {
							hex_value += character;
							_read(fd, &character, sizeof(character));
						}
						if (character == ' ') {
							_read(fd, &character, sizeof(character));
						}

						std::stringstream stream;
						stream << std::hex << hex_value;

						unsigned int result;
						stream >> result;
						data.push_back(result);
					}

					// We'll have to "restore" this argument's data later on as the offset where the array will be written
					current->type = 0;
					unsigned int* actual_array = new unsigned int[data.size()];
					for (unsigned int i = 0; i < data.size(); i++) {
						actual_array[i] = data.at(i);
					}
					Data_Array data_array = {data.size(), actual_array};
					current->data_array = data_array;
					array_arguments.push_back(current);

					// Whatever it is, eat the next
					_lseek(fd, 1, SEEK_CUR);
				} else {
					// rewind since we've eaten away a meaningful char
					_lseek(fd, -1, SEEK_CUR);
					current->type = 0;
					current->raw_data = read_hex_string(fd, ' ');
				}

				arguments.push_back(current);
			}

			instructions.push_back(new Instruction(definition, arguments, data_array_end));

			if (definition->op_code == 0x3) {
				instr_3_offsets.insert(data_array_end);
			} else if (definition->op_code == 0x71) {
				instr_71_offsets.insert(data_array_end);
			} else if (definition->op_code == 0x8F) {
				instr_8f_offsets.insert(data_array_end);
			}

			data_array_end += compute_length(*definition);
		}

		instruction_label = read_instruction_label(fd);
	}

	_close(fd);

	// Before writing our instructions, we need to restore the label, string and array offsets
	for (std::vector<Argument*>::iterator iterator = label_arguments.begin(); iterator != label_arguments.end(); ++iterator) {
		unsigned int offset = label_to_offset[(*iterator)->raw_data];
		(*iterator)->raw_data = (offset - HEADER_LENGTH) >> 2;
	}
	
	// Restore the strings offsets
	std::basic_stringstream<unsigned char> string_data;
	unsigned current_string_offset = data_array_end;
	for (std::vector<Argument*>::iterator iterator = string_arguments.begin(); iterator != string_arguments.end(); ++iterator) {
		(*iterator)->raw_data = (current_string_offset - HEADER_LENGTH) >> 2;
		// we have at least one 0xFF as a separator, + as many as needed to reach a multiple of four for the next offset.
		current_string_offset = current_string_offset + (*iterator)->decoded_string.length() + 1;
		for (unsigned int i = 0; i < (*iterator)->decoded_string.length(); i++) {
			unsigned char c = (*iterator)->decoded_string[i] ^ 0xFF;
			string_data << c;
		}
		int padding = 4 - ((current_string_offset) % 4);
		for (int i = 0; i < padding + 1; i++) {
			unsigned char padding_char = 0xFF;
			string_data << padding_char;
		}
		current_string_offset += padding;
	}
	string_data.flush();

	// assemble the offset indexing of the footer
	std::vector<unsigned int> footer_data;
	// Restore the array offsets
	unsigned int current_array_offset = (current_string_offset - HEADER_LENGTH) >> 2;
	for (std::vector<Argument*>::iterator iterator = array_arguments.begin(); iterator != array_arguments.end(); ++iterator) {
		(*iterator)->raw_data = current_array_offset;
		footer_data.push_back((*iterator)->data_array.length);
		current_array_offset += (*iterator)->data_array.length + 1;
		for (unsigned int i = 0; i < (*iterator)->data_array.length; i++) {
			footer_data.push_back((*iterator)->data_array.data[i]);
		}
	}

	for (std::set<unsigned int>::iterator iterator = instr_71_offsets.begin(); iterator != instr_71_offsets.end(); ++iterator) {
		footer_data.push_back((*iterator - HEADER_LENGTH) >> 2);
	}
	header.table_1_length = instr_71_offsets.size();
	header.table_1_offset = current_array_offset;
	for (std::set<unsigned int>::iterator iterator = instr_3_offsets.begin(); iterator != instr_3_offsets.end(); ++iterator) {
		footer_data.push_back((*iterator - HEADER_LENGTH) >> 2);
	}
	header.table_2_length = instr_3_offsets.size();
	header.table_2_offset = header.table_1_offset + header.table_1_length;
	for (std::set<unsigned int>::iterator iterator = instr_8f_offsets.begin(); iterator != instr_8f_offsets.end(); ++iterator) {
		footer_data.push_back((*iterator - HEADER_LENGTH) >> 2);
	}
	header.table_3_length = instr_8f_offsets.size();
	header.table_3_offset = header.table_2_offset + header.table_2_length;

	const std::basic_string<unsigned char> strings = string_data.str();
	write_assembled_file(output, header, instructions, strings, footer_data);
	
	return 0;
}