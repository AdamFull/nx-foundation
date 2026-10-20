#include "core/foundation/serialization/arguments.h"

#include "core/foundation/strings/format.h"

#include <algorithm>

namespace nx::cli {
namespace {

[[maybe_unused, nodiscard]] bool
valid_name(const nx::string_view name) noexcept {
  if (name.empty())
    return false;
  for (const char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  return true;
}

[[maybe_unused, nodiscard]] bool
valid_positional_name(const nx::string_view name) noexcept {
  if (name.empty())
    return false;
  for (const char c : name)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
      return false;
  return true;
}

[[maybe_unused, nodiscard]] bool valid_short_name(const char name) noexcept {
  return name == '\0' ||
         ((name >= 'a' && name <= 'z') || (name >= 'A' && name <= 'Z') ||
          (name >= '0' && name <= '9'));
}

[[nodiscard]] nx::string quoted(const nx::string_view value) {
  return nx::format("'{}'", value);
}

void append_table(nx::string &out,
                  const nx::vector<std::pair<nx::string, nx::string>> &rows) {
  usize width = 0;
  for (const auto &[label, description] : rows) {
    (void)description;
    width = nx::max(width, label.size());
  }
  for (const auto &[label, description] : rows) {
    out += label;
    out.append(width - label.size() + 2, ' ');
    out += description;
    out += '\n';
  }
}

} // namespace

Command::Command(const nx::string_view name, const nx::string_view description)
    : m_name(name), m_description(description) {}

Command &Command::flag(const nx::string_view name, bool &output,
                       const nx::string_view description,
                       const FlagOptions options) {
  NX_ASSERT(valid_name(name));
  NX_ASSERT(name != "help");
  NX_ASSERT(valid_short_name(options.short_name));
  NX_ASSERT(options.short_name != 'h');
  for ([[maybe_unused]] const Argument &argument : m_arguments) {
    NX_ASSERT(argument.name.view() != name);
    NX_ASSERT(options.short_name == '\0' ||
              argument.short_name != options.short_name);
  }
  Argument argument;
  argument.kind = Kind::Flag;
  argument.name = nx::string(name);
  argument.description = nx::string(description);
  argument.short_name = options.short_name;
  argument.terminal = options.terminal;
  argument.output = &output;
  m_arguments.push_back(std::move(argument));
  return *this;
}

void Command::add_value(const nx::string_view name, void *const output,
                        const detail::AssignValue assign,
                        const nx::string_view description,
                        const ValueOptions &options, const bool repeatable) {
  NX_ASSERT(valid_name(name));
  NX_ASSERT(name != "help");
  NX_ASSERT(valid_short_name(options.short_name));
  NX_ASSERT(options.short_name != 'h');
  NX_ASSERT(!options.value_name.empty());
  NX_ASSERT(output != nullptr && assign != nullptr);
  for ([[maybe_unused]] const Argument &argument : m_arguments) {
    NX_ASSERT(argument.name.view() != name);
    NX_ASSERT(options.short_name == '\0' ||
              argument.short_name != options.short_name);
  }
  Argument argument;
  argument.kind = Kind::Option;
  argument.name = nx::string(name);
  argument.description = nx::string(description);
  argument.value_name = nx::string(options.value_name);
  argument.short_name = options.short_name;
  argument.required = options.required;
  argument.allow_empty = options.allow_empty;
  argument.repeatable = repeatable;
  argument.output = output;
  argument.assign = assign;
  for (const nx::string_view choice : options.choices) {
    NX_ASSERT(!choice.empty());
    argument.choices.emplace_back(choice);
  }
  m_arguments.push_back(std::move(argument));
}

void Command::add_positional(const nx::string_view name, void *const output,
                             const detail::AssignValue assign,
                             const nx::string_view description,
                             const PositionalOptions &options,
                             const bool repeatable) {
  NX_ASSERT(valid_positional_name(name));
  NX_ASSERT(output != nullptr && assign != nullptr);
  [[maybe_unused]] bool optional_seen = false;
  for (const Argument &argument : m_arguments) {
    NX_ASSERT(argument.name.view() != name);
    if (argument.kind != Kind::Positional)
      continue;
    NX_ASSERT(!argument.repeatable && "a repeated positional must be last");
    optional_seen |= !argument.required;
  }
  NX_ASSERT(!(optional_seen && options.required) &&
            "a required positional cannot follow an optional positional");

  Argument argument;
  argument.kind = Kind::Positional;
  argument.name = nx::string(name);
  argument.description = nx::string(description);
  argument.value_name = nx::string(name);
  argument.required = options.required;
  argument.allow_empty = options.allow_empty;
  argument.repeatable = repeatable;
  argument.output = output;
  argument.assign = assign;
  for (const nx::string_view choice : options.choices) {
    NX_ASSERT(!choice.empty());
    argument.choices.emplace_back(choice);
  }
  m_arguments.push_back(std::move(argument));
}

Parser::Parser(const nx::string_view program, const nx::string_view description)
    : m_program(program), m_description(description) {
  NX_ASSERT(!program.empty());
}

Command &Parser::command(const nx::string_view name,
                         const nx::string_view description) {
  NX_ASSERT(valid_name(name));
  NX_ASSERT(find_command(name) == nullptr);
  m_commands.push_back(nx::make_unique<Command>(name, description));
  return *m_commands.back();
}

Command *Parser::find_command(const nx::string_view name) noexcept {
  for (const nx::unique_ptr<Command> &command : m_commands)
    if (command->m_name.view() == name)
      return command.get();
  return nullptr;
}

const Command *Parser::find_command(const nx::string_view name) const noexcept {
  for (const nx::unique_ptr<Command> &command : m_commands)
    if (command->m_name.view() == name)
      return command.get();
  return nullptr;
}

nx::string Parser::option_label(const Command::Argument &argument) {
  nx::string out = "  ";
  if (argument.short_name != '\0') {
    out += '-';
    out += argument.short_name;
    out += ", ";
  } else {
    out += "    ";
  }
  out += "--";
  out += argument.name;
  if (argument.kind == Command::Kind::Option) {
    out += " <";
    out += argument.value_name;
    out += '>';
  }
  return out;
}

bool Parser::choice_allowed(const Command::Argument &argument,
                            const nx::string_view value) noexcept {
  if (argument.choices.empty())
    return true;
  return std::any_of(
      argument.choices.begin(), argument.choices.end(),
      [value](const nx::string &choice) { return choice.view() == value; });
}

nx::string Parser::choices_text(const Command::Argument &argument) {
  nx::string out;
  for (usize i = 0; i < argument.choices.size(); ++i) {
    if (i != 0)
      out += ", ";
    out += argument.choices[i];
  }
  return out;
}

ParseResult Parser::parse(const int argc, const char *const *argv) {
  if (argc < 1 || argv == nullptr)
    return {ParseStatus::Error, {}, "argument vector is empty"};

  for (Command::Argument &argument : m_root.m_arguments)
    argument.seen = false;
  for (const nx::unique_ptr<Command> &command : m_commands)
    for (Command::Argument &argument : command->m_arguments)
      argument.seen = false;

  Command *selected = &m_root;
  int index = 1;
  if (index < argc) {
    const nx::string_view first(argv[index] != nullptr ? argv[index] : "");
    if (Command *const command = find_command(first)) {
      selected = command;
      ++index;
    }
  }

  auto failure = [selected](nx::string message) {
    return ParseResult{ParseStatus::Error, nx::string(selected->m_name.view()),
                       std::move(message)};
  };
  auto find_long = [selected](const nx::string_view name) {
    for (Command::Argument &argument : selected->m_arguments)
      if (argument.kind != Command::Kind::Positional &&
          argument.name.view() == name)
        return &argument;
    return static_cast<Command::Argument *>(nullptr);
  };
  auto find_short = [selected](const char name) {
    for (Command::Argument &argument : selected->m_arguments)
      if (argument.kind != Command::Kind::Positional &&
          argument.short_name == name)
        return &argument;
    return static_cast<Command::Argument *>(nullptr);
  };
  auto assign = [&failure](Command::Argument &argument,
                           const nx::string_view value,
                           const nx::string_view display) -> ParseResult {
    if (argument.seen && !argument.repeatable)
      return failure(nx::format("{} may only be specified once", display));
    if (value.empty() && !argument.allow_empty)
      return failure(nx::format("{} requires a non-empty value", display));
    if (!choice_allowed(argument, value)) {
      return failure(nx::format("invalid value {} for {} (expected one of: {})",
                                quoted(value), display,
                                choices_text(argument)));
    }
    nx::string reason;
    if (!argument.assign(value, argument.output, reason))
      return failure(nx::format("invalid value {} for {}: {}", quoted(value),
                                display, reason));
    argument.seen = true;
    return {ParseStatus::Parsed, {}, {}};
  };

  bool options_enabled = true;
  usize positional_index = 0;
  nx::vector<Command::Argument *> positionals;
  for (Command::Argument &argument : selected->m_arguments)
    if (argument.kind == Command::Kind::Positional)
      positionals.push_back(&argument);

  for (; index < argc; ++index) {
    const nx::string_view token(argv[index] != nullptr ? argv[index] : "");
    if (options_enabled && token == "--") {
      options_enabled = false;
      continue;
    }
    if (options_enabled && (token == "--help" || token == "-h"))
      return {ParseStatus::Help, nx::string(selected->m_name.view()), {}};

    Command::Argument *argument = nullptr;
    nx::string_view attached;
    bool has_attached = false;
    nx::string display;
    if (options_enabled && token.starts_with("--")) {
      const usize equals = token.find('=');
      const nx::string_view name = token.substr(
          2,
          equals == nx::string_view::npos ? nx::string_view::npos : equals - 2);
      if (name.empty())
        return failure("option name after '--' is empty");
      argument = find_long(name);
      if (argument == nullptr)
        return failure(nx::format("unknown option {}", quoted(token)));
      display = nx::format("'--{}'", name);
      if (equals != nx::string_view::npos)
        has_attached = true;
      if (has_attached)
        attached = token.substr(equals + 1);
    } else if (options_enabled && token.size() >= 2 && token.front() == '-') {
      if (token.size() != 2 && (token.size() < 3 || token[2] != '='))
        return failure(
            nx::format("unsupported short-option form {} (use '-x VALUE')",
                       quoted(token)));
      argument = find_short(token[1]);
      if (argument == nullptr)
        return failure(nx::format("unknown option {}", quoted(token)));
      display = nx::format("'-{}'", token[1]);
      if (token.size() >= 3)
        has_attached = true;
      if (has_attached)
        attached = token.substr(3);
    }

    if (argument != nullptr) {
      if (argument->kind == Command::Kind::Flag) {
        if (has_attached)
          return failure(nx::format("{} does not take a value", display));
        if (argument->seen)
          return failure(nx::format("{} may only be specified once", display));
        *static_cast<bool *>(argument->output) = true;
        argument->seen = true;
        if (argument->terminal) {
          return {ParseStatus::Parsed, nx::string(selected->m_name.view()), {}};
        }
        continue;
      }
      if (!has_attached) {
        if (++index >= argc)
          return failure(nx::format("{} requires a value", display));
        attached = argv[index] != nullptr ? argv[index] : "";
      }
      ParseResult result = assign(*argument, attached, display.view());
      if (!result.parsed())
        return result;
      continue;
    }

    if (positional_index >= positionals.size())
      return failure(nx::format("unexpected argument {}", quoted(token)));
    Command::Argument &positional = *positionals[positional_index];
    const nx::string positional_display =
        nx::format("argument '<{}>'", positional.name);
    ParseResult result = assign(positional, token, positional_display.view());
    if (!result.parsed())
      return result;
    if (!positional.repeatable)
      ++positional_index;
  }

  for (const Command::Argument &argument : selected->m_arguments) {
    if (!argument.required || argument.seen)
      continue;
    if (argument.kind == Command::Kind::Positional)
      return failure(
          nx::format("missing required argument '<{}>'", argument.name));
    return failure(nx::format("missing required option '--{}'", argument.name));
  }
  return {ParseStatus::Parsed, nx::string(selected->m_name.view()), {}};
}

nx::string Parser::help(const nx::string_view command_name) const {
  const Command *selected = &m_root;
  if (!command_name.empty()) {
    selected = find_command(command_name);
    if (selected == nullptr)
      return {};
  }

  nx::string out = "Usage: ";
  out += m_program;
  if (!selected->m_name.empty()) {
    out += ' ';
    out += selected->m_name;
  }
  bool has_options = false;
  for (const Command::Argument &argument : selected->m_arguments)
    has_options |= argument.kind != Command::Kind::Positional;
  if (has_options)
    out += " [options]";
  for (const Command::Argument &argument : selected->m_arguments) {
    if (argument.kind != Command::Kind::Positional)
      continue;
    out += argument.required ? " <" : " [<";
    out += argument.name;
    if (argument.repeatable)
      out += "...";
    out += argument.required ? ">" : ">]";
  }
  if (selected == &m_root && !m_commands.empty()) {
    out += "\n       ";
    out += m_program;
    out += " <command> [options]";
  }
  out += "\n";

  const nx::string_view description = selected == &m_root
                                          ? m_description.view()
                                          : selected->m_description.view();
  if (!description.empty()) {
    out += '\n';
    out += description;
    out += '\n';
  }

  if (selected == &m_root && !m_commands.empty()) {
    out += "\nCommands:\n";
    nx::vector<std::pair<nx::string, nx::string>> rows;
    rows.reserve(m_commands.size());
    for (const nx::unique_ptr<Command> &command : m_commands)
      rows.emplace_back(nx::format("  {}", command->m_name),
                        command->m_description);
    append_table(out, rows);
  }

  nx::vector<std::pair<nx::string, nx::string>> positional_rows;
  nx::vector<std::pair<nx::string, nx::string>> option_rows;
  for (const Command::Argument &argument : selected->m_arguments) {
    nx::string argument_description = argument.description;
    if (!argument.choices.empty()) {
      if (!argument_description.empty())
        argument_description += ' ';
      argument_description +=
          nx::format("(choices: {})", choices_text(argument));
    }
    if (argument.required && argument.kind != Command::Kind::Positional)
      argument_description += " (required)";
    if (argument.kind == Command::Kind::Positional) {
      positional_rows.emplace_back(nx::format("  <{}>", argument.name),
                                   std::move(argument_description));
    } else {
      option_rows.emplace_back(option_label(argument),
                               std::move(argument_description));
    }
  }
  if (!positional_rows.empty()) {
    out += "\nArguments:\n";
    append_table(out, positional_rows);
  }
  option_rows.emplace_back("  -h, --help", "Show this help and exit.");
  out += "\nOptions:\n";
  append_table(out, option_rows);
  return out;
}

} // namespace nx::cli
