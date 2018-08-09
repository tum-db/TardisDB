
#pragma once

#include <string>

void benchmark_arithmetic_1_null(unsigned runs);

void benchmark_arithmetic_2_null(unsigned runs);

void benchmark_arithmetic_3_null(unsigned runs);

void benchmark_arithmetic_4_null(unsigned runs);

void benchmark_tpc_h_1(unsigned runs);

void benchmark_tpc_h_1_null(unsigned runs);

void benchmarkNamedQuery(const std::string & queryName, unsigned runs);

void run_tpc_h_1();

void run_tpc_h_1_null();

void runNamedQuery(const std::string & queryName);
