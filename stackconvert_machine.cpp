#include <iostream>
#include <thread>

#include "optimise.hpp"
#include "register_convert.hpp"
#include "stackconvert_machine.hpp"
#include "util.hpp"

void convertmachine::run_reg(const dcpu16::program &prog, bool verbose, bool speedlimit, bool optimise)
{
	this->terminate = false;
	this->reg_prog = prog;
	size_t skip = 0;
	this->pc = 0;
	for (uint16_t reg_pc = 0; !this->terminate && reg_pc < this->reg_prog.size();) {
		auto start = std::chrono::high_resolution_clock::now();

		/* Get cached instruction snippet, if it exists. Otherwise create it */
		j5::program snippet;
		uint16_t distance = 0;
		auto section_it = this->section_cache.find(reg_pc);
		if (section_it != this->section_cache.end()) {
			snippet = section_it->second.first;
			distance = section_it->second.second;
		} else {
			// find end of section (next label)
			auto next_label = std::find_if(
				this->reg_prog.begin() + reg_pc + 1,
				this->reg_prog.end(),
				[](const dcpu16::instruction &i){return !i.label.empty();}
			);
			distance = std::distance(this->reg_prog.begin() + reg_pc, next_label);
			if (verbose) {
				std::cout << "# Caching " << this->reg_prog.at(reg_pc) << " (" <<  distance << ")\n";
			}
			snippet = convert_instructions(this->reg_prog.begin() + reg_pc, next_label);
			if (optimise) snippet = optimise_instructions(snippet);
			this->section_cache[reg_pc] = {snippet, distance};
		}

		/* Run instruction snippet */
		std::cerr << prog.at(reg_pc) << '\n';
		for (size_t start_pc = this->pc;  this->pc - start_pc < snippet.size(); this->pc++) {
			const auto &i = snippet.at(this->pc - start_pc);
			if (skip > 0) {
				skip--;
				continue;
			}
			std::cerr << '\t' << i << '\n';
			auto new_pc = this->run_instruction(i);

			bool breakout = false;
			if (verbose) std::cout << this->register_dump() << '\n';
			// branch specials
			switch (i.code) {
				case j5::op_t::BRZERO:
					skip = new_pc - this->pc - 1; // get rid of relative
					break;
				case j5::op_t::BRANCH: {
					std::string branch_label = boost::get<std::string>(i.op);
					if (snippet.begin()->label == branch_label) {
						// label is in current snippet
						this->pc = start_pc - 1; // loop
					} else {
						reg_pc = new_pc - distance; // postinc
						breakout = true;
					}
					break;
				}
				default:
					break;
			}
			if (breakout) break;
		}
		if (verbose) std::cout << '\n';

		if (speedlimit) {
			std::this_thread::sleep_until(start + std::chrono::milliseconds(100)); // arbitrary
		}
		reg_pc += distance;
	}
}

uint16_t convertmachine::find_label(const std::string &l)
{
	auto pos = std::find_if(this->reg_prog.begin(), this->reg_prog.end(), [l](const dcpu16::instruction &i) { return i.label == l; });
	if (pos == this->reg_prog.end()) {
		throw "Undefined label '" + l + "' used";
	}
	return std::distance(this->reg_prog.begin(), pos);
}

