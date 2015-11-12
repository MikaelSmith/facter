#include <facter/version.h>
#include <facter/logging/logging.hpp>
#include <facter/facts/collection.hpp>
#include <facter/ruby/ruby.hpp>
#include <leatherman/util/scope_exit.hpp>
#include <leatherman/locale/locale.hpp>
#include <boost/algorithm/string.hpp>
// Note the caveats in nowide::cout/cerr; they're not synchronized with stdio.
// Thus they can't be relied on to flush before program exit.
// Use endl/ends or flush to force synchronization when necessary.
#include <boost/nowide/iostream.hpp>
#include <boost/nowide/args.hpp>

// boost includes are not always warning-clean. Disable warnings that
// cause problems before including the headers, then re-enable the warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include <boost/program_options.hpp>
#pragma GCC diagnostic pop

#include <iostream>
#include <set>
#include <algorithm>
#include <iterator>

#define _(x) leatherman::locale::translate(x).c_str()

using namespace std;
using namespace facter::facts;
using namespace facter::logging;
namespace po = boost::program_options;

void help(po::options_description& desc)
{
    boost::nowide::cout <<
        _("Synopsis\n") <<
        "========\n"
        "\n" <<
        _("Collect and display facts about the system.\n") <<
        "\n" <<
        _("Usage\n") <<
        "=====\n"
        "\n" <<
        _("  facter [options] [query] [query] [...]\n") <<
        "\n" <<
        _("Options\n") <<
        "=======\n\n" << desc <<
        _("\nDescription\n") <<
        "===========\n"
        "\n" <<
        _("Collect and display facts about the current system.  The library behind\n"
        "facter is easy to extend, making facter an easy way to collect information\n"
        "about a system.\n") <<
        "\n" <<
        _("If no queries are given, then all facts will be returned.\n") <<
        "\n" <<
        _("Example Queries\n") <<
        "===============\n\n" <<
        _("  facter kernel\n") <<
        _("  facter networking.ip\n") <<
        _("  facter processors.models.0") << endl;
}

void log_command_line(int argc, char** argv)
{
    if (!is_enabled(level::info)) {
        return;
    }
    ostringstream command_line;
    for (int i = 1; i < argc; ++i) {
        if (command_line.tellp() != static_cast<streampos>(0)) {
            command_line << ' ';
        }
        command_line << argv[i];
    }
    log(level::info, _("executed with command line: %1%."), command_line.str());
}

void log_queries(set<string> const& queries)
{
    if (!is_enabled(level::info)) {
        return;
    }

    if (queries.empty()) {
        log(level::info, _("resolving all facts."));
        return;
    }

    ostringstream output;
    for (auto const& query : queries) {
        if (query.empty()) {
            continue;
        }
        if (output.tellp() != static_cast<streampos>(0)) {
            output << ' ';
        }
        output << query;
    }
    log(level::info, _("requested queries: %1%."), output.str());
}

int main(int argc, char **argv)
{
    try
    {
        // Fix args on Windows to be UTF-8
        boost::nowide::args arg_utf8(argc, argv);

        // Setup logging
        boost::nowide::cout.imbue(leatherman::locale::get_locale());
        setup_logging(boost::nowide::cerr);

        vector<string> external_directories;
        vector<string> custom_directories;

        auto help_ = leatherman::locale::translate("help");

        // Build a list of options visible on the command line
        // Keep this list sorted alphabetically
        po::options_description visible_options("");
        visible_options.add_options()
            (_("color"), _("Enables color output."))
            (_("custom-dir"), po::value<vector<string>>(&custom_directories), _("A directory to use for custom facts."))
            (_("debug,d"), _("Enable debug output."))
            (_("external-dir"), po::value<vector<string>>(&external_directories), _("A directory to use for external facts."))
            (help_.c_str(), _("Print this help message."))
            (_("json,j"), _("Output in JSON format."))
            (_("show-legacy"), _("Show legacy facts when querying all facts."))
            (_("log-level,l"), po::value<level>()->default_value(level::warning, "warn"), _("Set logging level.\nSupported levels are: none, trace, debug, info, warn, error, and fatal."))
            (_("no-color"), _("Disables color output."))
            (_("no-custom-facts"), _("Disables custom facts."))
            (_("no-external-facts"), _("Disables external facts."))
            (_("no-ruby"), _("Disables loading Ruby, facts requiring Ruby, and custom facts."))
            (_("puppet,p"), _("(Deprecated: use `puppet facts` instead) Load the Puppet libraries, thus allowing Facter to load Puppet-specific facts."))
            (_("trace"), _("Enable backtraces for custom facts."))
            (_("verbose"), _("Enable verbose (info) output."))
            (_("version,v"), _("Print the version and exit."))
            (_("yaml,y"), _("Output in YAML format."));

        // Build a list of "hidden" options that are not visible on the command line
        po::options_description hidden_options("");
        hidden_options.add_options()
            ("query", po::value<vector<string>>());
        if (help_ != "help") {
            hidden_options.add_options()("help", "");
        }

        // Create the supported command line options (visible + hidden)
        po::options_description command_line_options;
        command_line_options.add(visible_options).add(hidden_options);

        // Build a list of positional options (in our case, just queries)
        po::positional_options_description positional_options;
        positional_options.add(_("query"), -1);

        po::variables_map vm;
        try {
            po::store(po::command_line_parser(argc, argv).
                      options(command_line_options).positional(positional_options).run(), vm);

            // Check for a help option first before notifying
            if (vm.count(help_) || vm.count("help")) {
                help(visible_options);
                return EXIT_SUCCESS;
            }

            po::notify(vm);

            // Check for conflicting options
            if (vm.count(_("color")) && vm.count(_("no-color"))) {
                throw po::error(_("color and no-color options conflict: please specify only one."));
            }
            if (vm.count(_("json")) && vm.count(_("yaml"))) {
                throw po::error(_("json and yaml options conflict: please specify only one."));
            }
            if (vm.count(_("no-external-facts")) && vm.count(_("external-dir"))) {
                throw po::error(_("no-external-facts and external-dir options conflict: please specify only one."));
            }
            if (vm.count(_("no-custom-facts")) && vm.count(_("custom-dir"))) {
                throw po::error(_("no-custom-facts and custom-dir options conflict: please specify only one."));
            }
            if ((vm.count(_("debug")) + vm.count(_("verbose")) + (vm[_("log-level")].defaulted() ? 0 : 1)) > 1) {
                throw po::error(_("debug, verbose, and log-level options conflict: please specify only one."));
            }
            if (vm.count(_("no-ruby")) && vm.count(_("custom-dir"))) {
                throw po::error(_("no-ruby and custom-dir options conflict: please specify only one."));
            }
            if (vm.count(_("puppet")) && vm.count(_("no-custom-facts"))) {
                throw po::error(_("puppet and no-custom-facts options conflict: please specify only one."));
            }
            if (vm.count(_("puppet")) && vm.count(_("no-ruby"))) {
                throw po::error(_("puppet and no-ruby options conflict: please specify only one."));
            }
        }
        catch (exception& ex) {
            colorize(boost::nowide::cerr, level::error);
            boost::nowide::cerr << "error: " << ex.what() << "\n" << endl;
            colorize(boost::nowide::cerr);
            help(visible_options);
            return EXIT_FAILURE;
        }

        // Check for printing the version
        if (vm.count(_("version"))) {
            boost::nowide::cout << LIBFACTER_VERSION_WITH_COMMIT << endl;
            return EXIT_SUCCESS;
        }

        // Set colorization; if no option was specified, use the default
        if (vm.count(_("color"))) {
            set_colorization(true);
        } else if (vm.count(_("no-color"))) {
            set_colorization(false);
        }

        // Get the logging level
        auto lvl= vm[_("log-level")].as<level>();
        if (vm.count(_("debug"))) {
            lvl = level::debug;
        } else if (vm.count(_("verbose"))) {
            lvl = level::info;
        }
        set_level(lvl);

        log_command_line(argc, argv);

        // Initialize Ruby in main
        bool ruby = (vm.count(_("no-ruby")) == 0) && facter::ruby::initialize(vm.count(_("trace")) == 1);
        leatherman::util::scope_exit ruby_cleanup{[ruby]() {
            if (ruby) {
                facter::ruby::uninitialize();
            }
        }};

        // Build a set of queries from the command line
        set<string> queries;
        if (vm.count(_("query"))) {
            for (auto const &q : vm[_("query")].as<vector<string>>()) {
                // Strip whitespace and query delimiter
                string query = boost::trim_copy_if(q, boost::is_any_of(".") || boost::is_space());

                // Erase any duplicate consecutive delimiters
                query.erase(unique(query.begin(), query.end(), [](char a, char b) {
                    return a == b && a == '.';
                }), query.end());

                // Don't insert empty queries
                if (query.empty()) {
                    continue;
                }

                queries.emplace(move(query));
            }
        }

        log_queries(queries);

        collection facts;
        facts.add_default_facts(ruby);

        if (!vm.count(_("no-external-facts"))) {
            facts.add_external_facts(external_directories);
        }

        // Add the environment facts
        facts.add_environment_facts();

        if (ruby && !vm.count(_("no-custom-facts"))) {
            facter::ruby::load_custom_facts(facts, vm.count(_("puppet")), custom_directories);
        }

        // Output the facts
        format fmt = format::hash;
        if (vm.count("json")) {
            fmt = format::json;
        } else if (vm.count(_("yaml"))) {
            fmt = format::yaml;
        }

        bool show_legacy = vm.count(_("show-legacy"));
        facts.write(boost::nowide::cout, fmt, queries, show_legacy);
        boost::nowide::cout << endl;
    } catch (locale_error const& e) {
        boost::nowide::cerr << "failed to initialize logging system due to a locale error: " << e.what() << "\n" << endl;
        return 2;  // special error code to indicate we failed harder than normal
    } catch (exception& ex) {
        log(level::fatal, "unhandled exception: %1%", ex.what());
    }

    return error_logged() ? EXIT_FAILURE : EXIT_SUCCESS;
}
