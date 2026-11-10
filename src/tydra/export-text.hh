// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <streambuf>
#include <string>
namespace lightusd {
namespace tydra {
namespace export_detail {
class TextBuffer final : public std::streambuf {
 public:
  explicit TextBuffer(size_t limit) : limit_(limit) {}
  std::string text;

 protected:
  std::streamsize xsputn(const char* data, std::streamsize count) override {
    if (count < 0 || static_cast<size_t>(count) > limit_ - text.size())
      return 0;
    text.append(data, static_cast<size_t>(count));
    return count;
  }
  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof()))
      return traits_type::not_eof(value);
    const char c = traits_type::to_char_type(value);
    return xsputn(&c, 1) == 1 ? value : traits_type::eof();
  }

 private:
  size_t limit_;
};
}  // namespace export_detail
}  // namespace tydra
}  // namespace lightusd
