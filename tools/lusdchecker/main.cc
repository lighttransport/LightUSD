// SPDX-License-Identifier: Apache-2.0
#include "checker.hh"
int main(int argc, char** argv) {
  return lusdchecker::RunChecker(argc, argv,
      lightusd::next::GetBuiltinValidationRegistry());
}
