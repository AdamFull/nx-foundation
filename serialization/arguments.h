#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include <charconv>
#include <initializer_list>
#include <string>
#include <type_traits>

namespace nx::cli {

/// Outcome of parsing a command-line invocation.
enum class ParseStatus : u8 { Parsed, Help, Error };

/// Parsed command name and, on failure, a user-facing diagnostic.
struct ParseResult {
  ParseStatus status = ParseStatus::Error;
  nx::string command;
  nx::string message;

  [[nodiscard]] bool parsed() const noexcept {
    return status == ParseStatus::Parsed;
  }
};

/// Configuration for a named option that consumes a value.
struct ValueOptions {
  nx::string_view value_name = "VALUE";
  char short_name = '\0';
  bool required = false;
  bool allow_empty = false;
  std::initializer_list<nx::string_view> choices =
      std::initializer_list<nx::string_view>();
};

/// Configuration for a boolean flag.
struct FlagOptions {
  char short_name = '\0';
  bool terminal = false;
};

/// Configuration for a positional argument.
struct PositionalOptions {
  bool required = true;
  bool allow_empty = false;
  std::initializer_list<nx::string_view> choices =
      std::initializer_list<nx::string_view>();
};

namespace detail {

template <typename> inline constexpr bool dependent_false = false;

template <typename T>
[[nodiscard]] bool parse_argument_value(const nx::string_view text, T &out,
                                        nx::string &reason) {
  using Value = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<Value, nx::string>) {
    out = nx::string(text);
    return true;
  } else if constexpr (std::is_same_v<Value, nx::string_view>) {
    out = text;
    return true;
  } else if constexpr (std::is_same_v<Value, std::string>) {
    out.assign(text.data(), text.size());
    return true;
  } else if constexpr (std::is_same_v<Value, bool>) {
    if (text == "true" || text == "on" || text == "1") {
      out = true;
      return true;
    }
    if (text == "false" || text == "off" || text == "0") {
      out = false;
      return true;
    }
    reason = "expected true/false, on/off, or 1/0";
    return false;
  } else if constexpr (std::is_integral_v<Value>) {
    Value parsed{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
      reason = "expected an integer in range";
      return false;
    }
    out = parsed;
    return true;
  } else if constexpr (std::is_floating_point_v<Value>) {
    Value parsed{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
      reason = "expected a number in range";
      return false;
    }
    out = parsed;
    return true;
  } else {
    static_assert(dependent_false<T>,
                  "unsupported CLI value type; parse into a supported scalar "
                  "and convert after parsing");
  }
}

using AssignValue = bool (*)(nx::string_view, void *, nx::string &);

template <typename T>
[[nodiscard]] bool assign_scalar(const nx::string_view text, void *output,
                                 nx::string &reason) {
  return parse_argument_value(text, *static_cast<T *>(output), reason);
}

template <typename T>
[[nodiscard]] bool assign_multiple(const nx::string_view text, void *output,
                                   nx::string &reason) {
  T parsed{};
  if (!parse_argument_value(text, parsed, reason))
    return false;
  static_cast<nx::vector<T> *>(output)->push_back(std::move(parsed));
  return true;
}

} // namespace detail

/// Declarative argument schema for either the root invocation or a subcommand.
///
/// Bound output values must outlive the owning Parser. Schema construction is
/// intended to finish before Parser::parse is called.
class Command {
public:
  Command(nx::string_view name = {}, nx::string_view description = {});

  Command &flag(nx::string_view name, bool &output,
                nx::string_view description = {}, FlagOptions options = {});

  /// Add a scalar named option. Strings, booleans, integral types, and
  /// floating-point types are supported and validated strictly.
  template <typename T>
  Command &option(nx::string_view name, T &output,
                  nx::string_view description = {}, ValueOptions options = {}) {
    add_value(name, &output, &detail::assign_scalar<T>, description, options,
              false);
    return *this;
  }

  template <typename T>
  Command &option_many(nx::string_view name, nx::vector<T> &output,
                       nx::string_view description = {},
                       ValueOptions options = {}) {
    add_value(name, &output, &detail::assign_multiple<T>, description, options,
              true);
    return *this;
  }

  template <typename T>
  Command &positional(nx::string_view name, T &output,
                      nx::string_view description = {},
                      PositionalOptions options = {}) {
    add_positional(name, &output, &detail::assign_scalar<T>, description,
                   options, false);
    return *this;
  }

  template <typename T>
  Command &positional_many(nx::string_view name, nx::vector<T> &output,
                           nx::string_view description = {},
                           PositionalOptions options = {}) {
    add_positional(name, &output, &detail::assign_multiple<T>, description,
                   options, true);
    return *this;
  }

  [[nodiscard]] nx::string_view name() const noexcept { return m_name.view(); }

private:
  friend class Parser;

  enum class Kind : u8 { Flag, Option, Positional };

  struct Argument {
    Kind kind = Kind::Flag;
    nx::string name;
    nx::string description;
    nx::string value_name;
    nx::vector<nx::string> choices;
    char short_name = '\0';
    bool required = false;
    bool allow_empty = false;
    bool repeatable = false;
    bool terminal = false;
    bool seen = false;
    void *output = nullptr;
    detail::AssignValue assign = nullptr;
  };

  void add_value(nx::string_view name, void *output, detail::AssignValue assign,
                 nx::string_view description, const ValueOptions &options,
                 bool repeatable);
  void add_positional(nx::string_view name, void *output,
                      detail::AssignValue assign, nx::string_view description,
                      const PositionalOptions &options, bool repeatable);

  nx::string m_name;
  nx::string m_description;
  nx::vector<Argument> m_arguments;
};

/// Owns a command-line schema, parses one invocation, and generates help text.
///
/// The root command can coexist with subcommands, allowing tools to retain a
/// default invocation while adding explicit commands. `--help` and `-h` are
/// reserved and handled automatically. User input errors are returned in a
/// ParseResult; invalid schemas are programmer errors checked by assertions.
class Parser {
public:
  explicit Parser(nx::string_view program, nx::string_view description = {});

  /// Return the default/root command schema.
  [[nodiscard]] Command &root() noexcept { return m_root; }

  /// Add and return a uniquely named subcommand schema.
  Command &command(nx::string_view name, nx::string_view description = {});

  /// Parse argv into the values bound by the selected command schema.
  [[nodiscard]] ParseResult parse(int argc, const char *const *argv);

  /// Generate help for the root command or a named subcommand.
  [[nodiscard]] nx::string help(nx::string_view command = {}) const;

private:
  [[nodiscard]] static nx::string
  option_label(const Command::Argument &argument);
  [[nodiscard]] static bool choice_allowed(const Command::Argument &argument,
                                           nx::string_view value) noexcept;
  [[nodiscard]] static nx::string
  choices_text(const Command::Argument &argument);
  [[nodiscard]] Command *find_command(nx::string_view name) noexcept;
  [[nodiscard]] const Command *
  find_command(nx::string_view name) const noexcept;

  nx::string m_program;
  nx::string m_description;
  Command m_root;
  nx::vector<nx::unique_ptr<Command>> m_commands;
};

} // namespace nx::cli
