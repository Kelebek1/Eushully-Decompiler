// Coded by Kellindil

#define HEADER_LENGTH 60

#include <string>
#include <vector>
#include <io.h>

const std::string HEADER_PREFIX = "==Binary Information - do not edit==\n";
const std::string HEADER_SUFFIX = "====\n\n";
const std::string SIGNATURE_PREFIX = "signature = ";
const std::string VARS_PREFIX = "\nlocal_vars = { ";
const std::string VARS_SUFFIX = " }\n";

struct BinaryHeader {
	unsigned char signature[8];

	unsigned int local_integer_1;
	unsigned int local_floats;
	unsigned int local_strings_1;
	unsigned int local_integer_2;
	unsigned int unknown_data;
	unsigned int local_strings_2;

	unsigned int sub_header_length; // Can this be anything other than 0x1C (size of the 6 integers below)?

	unsigned int table_1_length;
	unsigned int table_1_offset;

	unsigned int table_2_length;
	unsigned int table_2_offset;

	unsigned int table_3_length;
	unsigned int table_3_offset;
};

struct Data_Array {
	unsigned int length;
	unsigned int* data;
};

struct Argument {
	unsigned int type = 0;
	unsigned int raw_data = 0;
	std::string decoded_string = "";
	Data_Array data_array = {};
};

struct Instruction_Definition {
	unsigned int op_code;
	std::string label;
	unsigned int argument_count;
};

struct Instruction {
	Instruction_Definition* definition;
	std::vector<Argument*> arguments;
	unsigned int offset;

	Instruction(Instruction_Definition* def, std::vector<Argument*> args, unsigned int off) {
		definition = def;
		arguments = args;
		offset = off;
	}
};

int open_or_die(const std::filesystem::path& file_name, int flag, int mode = 0);

std::string sjis2utf8(const std::string& sjis);
std::string utf82sjis(const std::string& utf8);

Instruction_Definition* instruction_for_op_code(unsigned int op_code);

Instruction_Definition* instruction_for_label(std::string label);

inline bool is_control_flow(Instruction_Definition* instruction) {
	return instruction->op_code == 0x8C ||
		instruction->op_code == 0x8F ||
		instruction->op_code == 0xA0 ||
		// set code callbacks (UI buttons)
		instruction->op_code == 0xCC ||
		instruction->op_code == 0xFB ||
		// some call-looping thing
		instruction->op_code == 0xD4 ||
		instruction->op_code == 0x90 ||
		instruction->op_code == 0x7B;
}

inline bool is_control_flow(Instruction* instruction) {
	return is_control_flow(instruction->definition);
}

inline bool is_label_argument(Instruction* instruction, int x) {
	return ((instruction->definition->op_code == 0x8C || instruction->definition->op_code == 0x8F) && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
		(instruction->definition->op_code == 0xA0 && x > 0 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
		((instruction->definition->op_code == 0xCC || instruction->definition->op_code == 0xFB) && x > 0 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
		(instruction->definition->op_code == 0xD4 && x >= 2 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
		(instruction->definition->op_code == 0x90 && x >= 4 && instruction->arguments[x]->raw_data != 0xFFFFFFFF) ||
		(instruction->definition->op_code == 0x7B && instruction->arguments[x]->raw_data != 0xFFFFFFFF);
}

template <typename T, size_t N>
inline size_t size_of_array(const T(&)[N]) {
  return N;
}