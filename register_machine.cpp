#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "register_machine.hpp"

namespace dcpu16 {


bool is_hex(const std::string &s)
{
	return std::all_of(s.begin(), s.end(), ::isxdigit);
}

reg_t find_reg(const std::string &r)
{
	auto pos = std::find(REG_T_STR.begin(), REG_T_STR.end(), r);
	if (pos == REG_T_STR.end()) {
		throw "Not a register: " + r;
	}

	return static_cast<reg_t>(pos - REG_T_STR.begin());
}

std::ostream& operator<<(std::ostream &os, reg_t r)
{
	os << REG_T_STR.at(static_cast<size_t>(r));
	return os;
}

/**
 * Stream operator for printing an instruction to console
 * @param os The stream.
 * @param ins The instruction.
 * @return The modified stream.
 */
std::ostream& operator<<(std::ostream& os, const instruction& ins)
{
	if (!ins.label.empty()) os << ins.label << ": ";
	os << OP_T_STR.at((size_t)ins.code);
	os << " '" << ins.b << "'";
	if (!(ins.a.which() == 0 && boost::get<std::string>(ins.a).empty())) os << " '" << ins.a << "'";
	return os;
}

bool operator==(const instruction &a, const instruction &b)
{
	return a.code == b.code && a.b == b.b && a.a == b.a;
}

bool operator!=(const instruction &a, const instruction &b)
{
	return !(a == b);
}

const instruction instruction::NONE = {op_t::NUM_OPS, 0, 0, ""};


operand_t get_operand(const std::string &tok)
{
	operand_t ret;
	if (tok.size() > 2 && tok[0] == '0'
			&& tolower(tok[1]) == 'x' && is_hex(tok.substr(2))) {
		// hex literal
		ret = static_cast<uint16_t>(std::stoul(tok.substr(2), nullptr, 16));
	} else if (std::all_of(tok.begin(), tok.end(), ::isdigit)) {
		// decimal literal
		ret = static_cast<uint16_t>(std::stoul(tok));
	} else if (std::find(REG_T_STR.begin(), REG_T_STR.end(), tok) != REG_T_STR.end()) {
		// register (& pc, sp, etc)
		ret = find_reg(tok);
	} else if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') {
		ret = tok.substr(1, tok.length() - 2); // TODO: unescape control chars
	} else {
		// probably a string literal or label.
		ret = tok;
	}
	return ret;
}

instruction tokenise_line(const std::string &line)
{
	// Parse into separate words
	std::vector<std::string> words;
	bool inword = false;
	bool inquote = false;
	for (char c : line) {
		if (c == ';') break; // comment
		if (c == '"') inquote = !inquote;
		if (!inquote && (isspace(c) || c == ',')) {
			inword = false;
			continue;
		} else if (!inword) {
			words.emplace_back(); // start next word
		}
		words.back().push_back(c);
		inword = true;
	}

	if (words.empty()) return instruction::NONE;

	instruction ins;

	auto it = words.begin();
	if (it->front() == ':') {
		ins.label = it->substr(1);
		++it;
	}

	// Get operation
	auto opit = std::find(OP_T_STR.begin(), OP_T_STR.end(), *it);
	if (opit == OP_T_STR.end()) {
		throw "Unknown op code: " + *it;
	}
	ins.code = static_cast<op_t>(std::distance(OP_T_STR.begin(), opit));
	it++;

	// All instructions have at least one operand
	ins.b = get_operand(*it++);

	if (it != words.end()) {
		ins.a = get_operand(*it++);
	} else if (ins.code != op_t::OUT && ins.code != op_t::DAT) {
		throw "Incorrect number of operands for " + OP_T_STR.at((size_t)ins.code); // OUT & DAT have one operand
	}
	return ins;

}

program tokenise_source(const std::string &source)
{
	std::stringstream iss(source);

	program prog;
	std::string line;

	std::string::size_type pos = 0;
	std::string::size_type prev = 0;
	while ((pos = source.find('\n', prev)) != std::string::npos)
	{
		std::string line = source.substr(prev, pos - prev);
		instruction ins = tokenise_line(line);
		if (ins != instruction::NONE) prog.push_back(ins);
		prev = pos + 1;
	}
	return prog;
}

uint16_t machine::find_label(const std::string &l)
{
	auto pos = std::find_if(this->cur_prog.begin(), this->cur_prog.end(), [l](const instruction &i) { return i.label == l; });
	if (pos == this->cur_prog.end()) {
		throw "Undefined label '" + l + "' used";
	}
	return std::distance(this->cur_prog.begin(), pos);
}

uint16_t machine::get_val(const operand_t &x)
{
	switch (x.which()) {
		case 0: { // string
			// much sad.
			std::string op = boost::get<std::string>(x);
			if (op.size() > 2 && op.front() == '[' && op.back() == ']') { // array val
				std::string interior = op.substr(1, op.length() - 2);
				return this->mem.at(this->get_val(get_operand(interior)));
			} else if (op.find('+') != std::string::npos) { // expression. needs expanding to others
				size_t pos = op.find('+');
				std::string p1 = op.substr(0, pos);
				std::string p2 = op.substr(pos + 1);
				return this->get_val(get_operand(p1)) + this->get_val(get_operand(p2));
			} else {
				return this->find_label(op);
			}
		}
		case 1: // reg
			return this->regs.at(static_cast<size_t>(boost::get<reg_t>(x)));
		case 2: // literal
			return boost::get<uint16_t>(x);
	}
	throw "Could not get value??";
}

void machine::set_val(const operand_t &x, uint16_t val)
{
	switch (x.which()) {
		case 0: {
			// duplicates a lot of get_val
			std::string op = boost::get<std::string>(x);
			if (op.size() > 2 && op.front() == '[' && op.back() == ']') { // array val
				std::string interior = op.substr(1, op.length() - 2);
				this->mem.at(this->get_val(get_operand(interior))) = val;
			} else {
				throw "Could not find value to set?";
			}
			break;
		}
		case 1:
			this->regs.at(static_cast<size_t>(boost::get<reg_t>(x))) = val;
			break;
		case 2:
			break; // silently fail attempting to set a literal
	}
}

void machine::run(const program &prog, bool stack = false, bool verbose = false)
{
	this->cur_prog = prog;
	this->terminate = false;
	this->skip_next = false;
	this->regs[(size_t)reg_t::SP] = 0xffff;


	uint16_t &pc = this->regs[(size_t)reg_t::PC];
	for (; !this->terminate && pc < this->cur_prog.size(); pc++) {
		auto start = std::chrono::high_resolution_clock::now();

		if (skip_next) {
			skip_next = false;
			continue;
		}

		const auto &ins = this->cur_prog[pc];
		std::cerr << ins << '\n';
		switch (ins.code) {
			case op_t::OUT:
				this->out_func(ins.b);
				break;
			case op_t::DAT:
				this->dat_func(ins.b);
				break;
			// Bin ops
			case op_t::SET:
			case op_t::ADD:
			case op_t::SUB:
			case op_t::MUL:
			case op_t::MLI:
			case op_t::DIV:
			case op_t::DVI:
			case op_t::MOD:
			case op_t::MDI:
			case op_t::AND:
			case op_t::BOR:
			case op_t::XOR:
			case op_t::SHR:
			case op_t::ASR:
			case op_t::SHL:
			case op_t::ADX:
			case op_t::SBX: {
				uint16_t b = this->get_val(ins.b);
				uint16_t a = this->get_val(ins.a);
				auto func = BIN_OPS.at(ins.code);
				this->set_val(ins.b, func(this, b, a));
				break;
			}
			// Cond ops
			case op_t::IFB:
			case op_t::IFC:
			case op_t::IFE:
			case op_t::IFN:
			case op_t::IFG:
			case op_t::IFA:
			case op_t::IFL:
			case op_t::IFU: {
				uint16_t b = this->get_val(ins.b);
				uint16_t a = this->get_val(ins.a);
				auto func = COND_OPS.at(ins.code);
				this->skip_next = !func(b, a);
				break;
			}
			default:
				throw "Unrecognised instruction " + OP_T_STR.at((size_t)ins.code);
		}
		if (verbose) std::cout << this->register_dump() << '\n';
		std::this_thread::sleep_until(start + std::chrono::milliseconds(100)); // arbitrary
	}
}


template<typename ... Args>
std::string string_format(const std::string& format, Args... args)
{
	size_t size = std::snprintf(nullptr, 0, format.c_str(), args...) + 1; // Extra space for '\0'
	std::unique_ptr<char[]> buf(new char[size]);
	std::snprintf(buf.get(), size, format.c_str(), args...);
	return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

std::string machine::register_dump()
{
	uint16_t pc = this->regs[(size_t)reg_t::PC];
	uint16_t sp = this->regs[(size_t)reg_t::SP];
	uint16_t ia = this->regs[(size_t)reg_t::IA];
	uint16_t ex = this->regs[(size_t)reg_t::EX];

	uint16_t a = this->regs[(size_t)reg_t::A];
	uint16_t b = this->regs[(size_t)reg_t::B];
	uint16_t c = this->regs[(size_t)reg_t::C];
	uint16_t x = this->regs[(size_t)reg_t::X];
	uint16_t y = this->regs[(size_t)reg_t::Y];
	uint16_t z = this->regs[(size_t)reg_t::Z];
	uint16_t i = this->regs[(size_t)reg_t::I];
	uint16_t j = this->regs[(size_t)reg_t::J];
	return string_format("PC %04x SP %04x IA %04x EX %04x\n"
	                     "A  %04x B  %04x C  %04x\n"
	                     "X  %04x Y  %04x Z  %04x\n"
	                     "I  %04x J  %04x",
	                     pc, sp, ia, ex,
	                     a, b, c, x, y, z, i, j);

}


uint16_t machine::set_op(uint16_t, uint16_t a)
{
	return a;
}

uint16_t machine::add_op(uint16_t b, uint16_t a)
{
	uint32_t v = b + a;
	this->regs[static_cast<size_t>(reg_t::EX)] = v > 0xffff ? 0x1 : 0x0;
	return v;
}

uint16_t machine::sub_op(uint16_t b, uint16_t a)
{
	this->regs[static_cast<size_t>(reg_t::EX)] = a < b ? 0x1 : 0x0;
	return b - a;
}

uint16_t machine::mul_op(uint16_t b, uint16_t a)
{
	uint32_t v = b * a;
	this->regs[static_cast<size_t>(reg_t::EX)] = (v >> 16) & 0xffff;
	return v;
}

uint16_t machine::mli_op(uint16_t b, uint16_t a)
{
	uint32_t v = static_cast<int16_t>(b) * static_cast<int16_t>(a);
	this->regs[static_cast<size_t>(reg_t::EX)] = (v >> 16) & 0xffff;
	return v;
}

uint16_t machine::div_op(uint16_t b, uint16_t a)
{
	if (a == 0) {
		this->regs[static_cast<size_t>(reg_t::EX)] = 0x0;
		return 0;
	} else {
		this->regs[static_cast<size_t>(reg_t::EX)] = ((b << 16) / a) & 0xffff;
		return b / a;
	}
}

uint16_t machine::dvi_op(uint16_t b, uint16_t a)
{
	if (a == 0) {
		this->regs[static_cast<size_t>(reg_t::EX)] = 0x0;
		return 0;
	} else {
		this->regs[static_cast<size_t>(reg_t::EX)] = ((static_cast<int16_t>(b) << 16) / static_cast<int16_t>(a)) & 0xffff;
		return static_cast<int16_t>(b) / static_cast<int16_t>(a);
	}
}

uint16_t machine::mod_op(uint16_t b, uint16_t a)
{
	return a == 0 ? 0 : b % a;
}

uint16_t machine::mdi_op(uint16_t b, uint16_t a)
{
	return a == 0 ? 0 : static_cast<int16_t>(b) % static_cast<int16_t>(a);
}

uint16_t machine::and_op(uint16_t b, uint16_t a)
{
	return b & a;
}

uint16_t machine::bor_op(uint16_t b, uint16_t a)
{
	return b | a;
}

uint16_t machine::xor_op(uint16_t b, uint16_t a)
{
	return b ^ a;
}

uint16_t machine::shr_op(uint16_t b, uint16_t a)
{
	this->regs[static_cast<size_t>(reg_t::EX)] = (b << 16) >> a;
	return b >> a;
}

uint16_t machine::asr_op(uint16_t b, uint16_t a)
{
	this->regs[static_cast<size_t>(reg_t::EX)] = (static_cast<int16_t>(b) << 16) >> a;
	return static_cast<int16_t>(b) >> a;
}

uint16_t machine::shl_op(uint16_t b, uint16_t a)
{
	this->regs[static_cast<size_t>(reg_t::EX)] = (b << a) >> 16;
	return b << a;
}

uint16_t machine::adx_op(uint16_t b, uint16_t a)
{
	uint16_t ex = this->regs[static_cast<size_t>(reg_t::EX)];
	this->regs[static_cast<size_t>(reg_t::EX)] = (b + a + ex > 0xffff) ? 1 : 0;
	return b + a + ex;
}

uint16_t machine::sbx_op(uint16_t b, uint16_t a)
{
	uint16_t ex = this->regs[static_cast<size_t>(reg_t::EX)];
	this->regs[static_cast<size_t>(reg_t::EX)] = (a + ex < b) ? 0xffff : 0;
	return b - a + ex;
}


void machine::dat_func(operand_t x)
{
	if (x.which() == 2 && boost::get<uint16_t>(x) == 0) {
		this->terminate = true;
	}
}

void machine::out_func(operand_t x)
{
	uint16_t addr = this->get_val(x);
	switch (x.which()) {
		case 0:
			if (isalpha(boost::get<std::string>(x)[0])) { // points to a label
				std::cout << this->cur_prog.at(addr).b;
			} else {
				std::cout << std::to_string(addr);
			}
			break;
		case 1:
			std::cout << std::to_string(addr);
			break;
		case 2:
			std::cout << this->cur_prog.at(addr);
			break;
	}
}

}
