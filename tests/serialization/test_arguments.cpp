#include "framework/nxtest.h"

#include "core/foundation/serialization/arguments.h"

namespace {

template <usize N>
nx::cli::ParseResult parse(nx::cli::Parser &parser,
                           const char *const (&arguments)[N]) {
  return parser.parse(static_cast<int>(N), arguments);
}

} // namespace

TEST_CASE("arguments: root positionals and typed options parse") {
  nx::string source;
  i32 count = 0;
  bool enabled = false;
  nx::cli::Parser parser("tool", "A useful tool.");
  parser.root()
      .positional("source", source, "Input directory.")
      .option("count", count, "Item count.",
              {.value_name = "N", .short_name = 'n', .required = true})
      .flag("verbose", enabled, "Enable diagnostics.", {.short_name = 'v'});

  const char *const argv[] = {"tool", "assets", "-n", "42", "--verbose"};
  const nx::cli::ParseResult result = parse(parser, argv);
  REQUIRE(result.parsed());
  CHECK(result.command.empty());
  CHECK(source.view() == "assets");
  CHECK(count == 42);
  CHECK(enabled);
}

TEST_CASE(
    "arguments: subcommands, equals syntax, choices and strict booleans") {
  nx::string type;
  bool mips = true;
  nx::cli::Parser parser("assetc");
  parser.command("pack", "Pack one artifact.")
      .option("type", type, "Artifact kind.",
              {.required = true, .choices = {"texture", "shader"}})
      .option("mips", mips, "Generate mipmaps.");

  const char *const good[] = {"assetc", "pack", "--type=texture", "--mips",
                              "off"};
  const nx::cli::ParseResult parsed = parse(parser, good);
  REQUIRE(parsed.parsed());
  CHECK(parsed.command.view() == "pack");
  CHECK(type.view() == "texture");
  CHECK(!mips);

  const char *const bad_choice[] = {"assetc", "pack", "--type", "sound"};
  const nx::cli::ParseResult choice = parse(parser, bad_choice);
  CHECK(choice.status == nx::cli::ParseStatus::Error);
  CHECK(choice.message.find("texture, shader") != nx::string::npos);

  const char *const bad_bool[] = {"assetc",  "pack",   "--type",
                                  "texture", "--mips", "maybe"};
  const nx::cli::ParseResult boolean = parse(parser, bad_bool);
  CHECK(boolean.status == nx::cli::ParseStatus::Error);
  CHECK(boolean.message.find("true/false") != nx::string::npos);
}

TEST_CASE("arguments: required, duplicate, missing, and unknown values fail") {
  nx::string output;
  nx::cli::Parser parser("tool");
  parser.root().option("out", output, "Output.",
                       {.value_name = "FILE", .required = true});

  const char *const missing[] = {"tool"};
  CHECK(parse(parser, missing).message.view() ==
        "missing required option '--out'");

  const char *const no_value[] = {"tool", "--out"};
  CHECK(parse(parser, no_value).message.find("requires a value") !=
        nx::string::npos);

  const char *const duplicate[] = {"tool", "--out", "a", "--out", "b"};
  CHECK(parse(parser, duplicate).message.find("only be specified once") !=
        nx::string::npos);

  const char *const unknown[] = {"tool", "--other", "x"};
  CHECK(parse(parser, unknown).message.find("unknown option") !=
        nx::string::npos);

  const char *const explicitly_empty[] = {"tool", "--out="};
  CHECK(parse(parser, explicitly_empty).message.find("non-empty") !=
        nx::string::npos);
}

TEST_CASE("arguments: terminal flags bypass required application inputs") {
  nx::string source;
  bool version = false;
  nx::cli::Parser parser("tool");
  parser.root()
      .positional("source", source)
      .flag("version", version, "Print version.", {.terminal = true});

  const char *const argv[] = {"tool", "--version"};
  REQUIRE(parse(parser, argv).parsed());
  CHECK(version);
  CHECK(source.empty());
}

TEST_CASE("arguments: option terminator preserves dash-prefixed positionals") {
  nx::string value;
  nx::cli::Parser parser("tool");
  parser.root().positional("value", value);

  const char *const argv[] = {"tool", "--", "--literal"};
  REQUIRE(parse(parser, argv).parsed());
  CHECK(value.view() == "--literal");
}

TEST_CASE("arguments: repeated options and trailing positionals accumulate") {
  nx::vector<i32> values;
  nx::vector<nx::string> files;
  nx::cli::Parser parser("tool");
  parser.root()
      .option_many("value", values, "Values.", {.short_name = 'v'})
      .positional_many("file", files, "Files.");

  const char *const argv[] = {"tool", "-v", "1", "a", "-v=2", "b"};
  REQUIRE(parse(parser, argv).parsed());
  REQUIRE(values.size() == 2);
  CHECK(values[0] == 1);
  CHECK(values[1] == 2);
  REQUIRE(files.size() == 2);
  CHECK(files[0].view() == "a");
  CHECK(files[1].view() == "b");
}

TEST_CASE("arguments: generated root and command help describe the schema") {
  nx::string source;
  bool version = false;
  nx::cli::Parser parser("assetc", "Asset compiler.");
  parser.root()
      .positional("source", source, "Source tree.")
      .flag("version", version, "Print the version.");
  nx::string input;
  parser.command("pack", "Pack one artifact.")
      .option("in", input, "Input file.",
              {.value_name = "FILE", .required = true});

  const nx::string root = parser.help();
  CHECK(root.find("Usage: assetc") != nx::string::npos);
  CHECK(root.find("Commands:") != nx::string::npos);
  CHECK(root.find("pack") != nx::string::npos);
  CHECK(root.find("--version") != nx::string::npos);

  const nx::string command = parser.help("pack");
  CHECK(command.find("Usage: assetc pack [options]") != nx::string::npos);
  CHECK(command.find("--in <FILE>") != nx::string::npos);
  CHECK(command.find("(required)") != nx::string::npos);

  nx::string type;
  parser.command("inspect", "Inspect an artifact.")
      .option("type", type, "Artifact type.",
              {.choices = {"texture", "shader"}});
  CHECK(parser.help("inspect").find("choices: texture, shader") !=
        nx::string::npos);

  const char *const help[] = {"assetc", "pack", "--help"};
  const nx::cli::ParseResult result = parse(parser, help);
  CHECK(result.status == nx::cli::ParseStatus::Help);
  CHECK(result.command.view() == "pack");
}
