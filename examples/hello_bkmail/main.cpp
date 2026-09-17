/**
 * @file main.cpp
 * @brief Prints the bkmail version string.
 * @author Haoming Bai <haomingbai@hotmail.com>
 * @date   2026-09-15
 *
 * Copyright © 2026 Haoming Bai
 * SPDX-License-Identifier: MIT
 */

#include <bkmail/bkmail.h>

#include <iostream>

int main() {
  std::cout << "bkmail " << bkmail::version() << '\n';
  return 0;
}
