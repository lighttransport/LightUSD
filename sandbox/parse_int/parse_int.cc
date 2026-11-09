#include <vector>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <charconv>

#include "../../src/legacy/tiny-string.hh"

std::string gen_intarray(size_t n, bool delim_at_end) {
  std::stringstream ss;
  std::random_device rd;
  std::mt19937 engine(rd());
  std::uniform_int_distribution<int64_t> dist(-1000000, 1000000);

  ss << "[";
  for (size_t i = 0; i < n; i++) {
    int64_t val = dist(engine);
    ss << std::to_string(val);
    if (delim_at_end) {
      ss << ",";
    } else if (i < (n-1)) {
      ss << ",";
    }
  }
  ss << "]";

  return ss.str();
}

struct Lexer {
  void init(const char *_p_begin, const char *_p_end, size_t row = 0, size_t column = 0) {
    p_begin = _p_begin;
    p_end = _p_end;
    curr = p_begin;
    row_ = row;
    column_ = column;
  }

  void skip_whitespaces() {
    while (!eof()) {
      char s = *curr;
      if ((s == ' ') || (s == '\t') || (s == '\f') || (s == '\n') || (s == '\r') || (s == '\v')) {
        curr++;
        column_++;

        if (s == '\r') {
          if (!eof()) {
            char c{'\0'};
            look_char1(&c);
            if (c == '\n') {
              curr++;
            }
          }
          row_++;
          column_ = 0;
        } else if (s == '\n') {
          row_++;
          column_ = 0;
        }
      } else {
        break;
      }
    }
  }

  bool skip_until_delim_or_close_paren(const char delim, const char close_paren) {
    while (!eof()) {
      char s = *curr;
      if ((s == delim) || (s == close_paren)) {
        return true;
      }

      curr++;
      column_++;

      if (s == '\r') {
        if (!eof()) {
          char c{'\0'};
          look_char1(&c);
          if (c == '\n') {
            curr++;
          }
        }
        row_++;
        column_ = 0;
      } else if (s == '\n') {
        row_++;
        column_ = 0;
      }
    }

    return false;
  }

  bool char1(char *result) {
    if (eof()) {
      return false;
    }
    *result = *curr;
    curr++;
    column_++;

    if ((*result == '\r') || (*result == '\n')) {
       row_++;
       column_ = 0;
     }

    return true;
  }

  bool look_char1(char *result) {
    if (eof()) {
      return false;
    }
    *result = *curr;
    return true;
  }

  bool consume_char1() {
    if (eof()) {
      return false;
    }
    char c = *curr;
    curr++;

    if ((c == '\r') || (c == '\n')) {
       row_++;
       column_ = 0;
     }

    return true;
  }

  inline bool eof() const {
    return (curr >= p_end);
  }

  bool lex_int(uint16_t &len, bool &truncated) {
    constexpr size_t n_trunc_chars = 256;

    size_t n = 0;
    bool has_sign = false;
    bool found_digit = false;

    while (!eof() && (n < n_trunc_chars)) {
      char c;
      look_char1(&c);

      if ((c == '-') || (c == '+')) {
        if (has_sign || found_digit) {
          break;
        }
        has_sign = true;
      } else if ((c >= '0') && (c <= '9')) {
        found_digit = true;
      } else {
        break;
      }

      consume_char1();
      n++;
    }

    if (n == 0 || !found_digit) {
      len = 0;
      return false;
    }

    truncated = (n >= n_trunc_chars);
    len = uint16_t(n);
    return true;
  }

  void push_error(const std::string &msg) {
    err_ += msg + " (near line " + std::to_string(row_) + ", column " + std::to_string(column_) + ")\n";
  }

  std::string get_error() const {
    return err_;
  }

  const char *p_begin{nullptr};
  const char *p_end{nullptr};
  const char *curr{nullptr};
  size_t row_{0};
  size_t column_{0};

 private:
  std::string err_;
};

struct int_lex_span {
  const char *p_begin{nullptr};
  uint16_t length{0};
};

template<size_t N>
struct vec_lex_span {
  int_lex_span vspans[N];
};

bool lex_int_array(
  const char *p_begin,
  const char *p_end,
  std::vector<int_lex_span> &result,
  std::string &err,
  const bool allow_delim_at_last = true,
  const char delim = ',',
  const char open_paren = '[',
  const char close_paren = ']') {

  if (p_begin >= p_end) {
    err = "Invalid input\n";
    return false;
  }

  Lexer lexer;
  lexer.p_begin = p_begin;
  lexer.p_end = p_end;
  lexer.curr = p_begin;

  char c;
  if (!lexer.char1(&c)) {
    err = "Input too short.\n";
    return false;
  }

  if (c != open_paren) {
    err = "Input does not begin with open parenthesis character.\n";
    return false;
  }

  lexer.skip_whitespaces();

  while (!lexer.eof()) {
    bool prev_is_delim = false;

    {
      char c;
      if (!lexer.look_char1(&c)) {
        lexer.push_error("Invalid character found.");
        err = lexer.get_error();
        return false;
      }

      if (c == delim) {
        if (result.empty()) {
          lexer.push_error("Array element starts with the delimiter character.");
          err = lexer.get_error();
          return false;
        }
        prev_is_delim = true;
        lexer.consume_char1();
      }

      lexer.skip_whitespaces();
    }

    {
      char c;
      if (!lexer.look_char1(&c)) {
        lexer.push_error("Failed to read a character.");
        err = lexer.get_error();
        return false;
      }

      if (c == close_paren) {
        if (prev_is_delim) {
          if (allow_delim_at_last) {
            return true;
          } else {
            lexer.push_error("Delimiter character is not allowed before the closing parenthesis character.");
            err = lexer.get_error();
            return false;
          }
        } else {
          return true;
        }
      }
    }

    int_lex_span sp;
    sp.p_begin = lexer.curr;

    uint16_t length{0};
    bool truncated{false};

    if (!lexer.lex_int(length, truncated)) {
      lexer.push_error("Input is not an integer literal.");
      err = lexer.get_error();
      return false;
    }

    sp.length = length;

    if (truncated) {
      if (!lexer.skip_until_delim_or_close_paren(delim, close_paren)) {
        lexer.push_error("Failed to seek to delimiter or closing parenthesis character.");
        err = lexer.get_error();
        return false;
      }
    }

    result.emplace_back(std::move(sp));
    lexer.skip_whitespaces();
  }

  return true;
}

bool do_parse(
  uint32_t nthreads,
  const std::vector<int_lex_span> &spans,
  std::vector<int64_t> &results) {

  auto start = std::chrono::steady_clock::now();

  results.resize(spans.size());

  if (spans.size() > (1024*128)) {
    nthreads = (std::min)((std::max)(1u, nthreads), 256u);

    std::mutex mutex;
    std::atomic<size_t> cnt(0);
    std::atomic<bool> parse_failed{false};
    std::vector<std::thread> threads;

    for (uint32_t i = 0; i < nthreads; i++) {
      threads.emplace_back(std::thread([&] {
        size_t j;

        while ((j = cnt++) < results.size()) {
          int64_t val;
          lightusd::tstring_view ts(spans[j].p_begin, size_t(spans[j].length));
          if (!lightusd::str::parse_int64(ts, &val)) {
            parse_failed = true;
          }

          results[j] = val;
        }
      }));
    }

    for (auto &&th : threads) {
      th.join();
    }

    if (parse_failed) {
      std::cerr << "parsing failure\n";
      return false;
    }

  } else {
    for (size_t i = 0; i < spans.size(); i++) {
      int64_t val;
      lightusd::tstring_view ts(spans[i].p_begin, size_t(spans[i].length));
      if (!lightusd::str::parse_int64(ts, &val)) {
        std::cerr << "parsing failure\n";
        return false;
      }

      results[i] = val;
    }
  }

  auto end = std::chrono::steady_clock::now();

  std::cout << "n threads: " << nthreads << "\n";
  std::cout << "n elems: " << spans.size() << "\n";
  std::cout << "parse time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " [ms]\n";

  return true;
}

int main(int argc, char **argv) {
  std::vector<int_lex_span> lex_results;

  uint32_t nthreads = 1;
  bool delim_at_end = true;
  size_t n = 1024*1024*32;

  if (argc > 1) {
    n = std::stoi(argv[1]);
  }
  if (argc > 2) {
    delim_at_end = std::stoi(argv[2]) > 0;
  }
  if (argc > 3) {
    nthreads = std::stoi(argv[3]);
  }

  lex_results.reserve(n);

  std::string input = gen_intarray(n, delim_at_end);

  auto start = std::chrono::steady_clock::now();

  std::string err;
  if (!lex_int_array(input.c_str(), input.c_str() + input.size(), lex_results, err)) {
    std::cerr << "parse error\n";
    std::cerr << err << "\n";
    return -1;
  }

  auto end = std::chrono::steady_clock::now();

  std::cout << "n elems " << lex_results.size() << "\n";
  std::cout << "size " << input.size() << "\n";
  std::cout << "lex time: " << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << " [ms]\n";

  std::vector<int64_t> parse_results;
  parse_results.reserve(n);

  do_parse(nthreads, lex_results, parse_results);

  return 0;
}
