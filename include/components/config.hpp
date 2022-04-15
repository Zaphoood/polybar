#pragma once

#include <unordered_map>

#include "common.hpp"
#include "components/logger.hpp"
#include "errors.hpp"
#include "settings.hpp"
#include "utils/env.hpp"
#include "utils/file.hpp"
#include "utils/string.hpp"
#if WITH_XRM
#include "x11/xresources.hpp"
#endif

POLYBAR_NS

DEFINE_ERROR(value_error);
DEFINE_ERROR(key_error);

using valuemap_t = std::unordered_map<string, string>;
using sectionmap_t = std::map<string, valuemap_t>;
using file_list = vector<string>;

class config {
 public:
  using make_type = const config&;
  static make_type make(string path = "", string bar = "");

  explicit config(const logger& logger, string&& path = "", string&& bar = "")
      : m_log(logger), m_file(move(path)), m_barname(move(bar)){};

  const string& filepath() const;
  string section() const;

  static constexpr const char* BAR_PREFIX = "bar/";

  /**
   * @brief Instruct the config to connect to the xresource manager
   */
  void use_xrm();

  void set_sections(sectionmap_t sections);

  void set_included(file_list included);

  void warn_deprecated(const string& section, const string& key, string replacement) const;

  /**
   * Returns true if a given parameter exists
   */
  bool has(const string& section, const string& key) const {
    auto it = m_sections.find(section);
    return it != m_sections.end() && it->second.find(key) != it->second.end();
  }

  /**
   * Set parameter value
   */
  void set(const string& section, const string& key, string&& value) {
    auto it = m_sections.find(section);
    if (it == m_sections.end()) {
      valuemap_t values;
      values[key] = value;
      m_sections[section] = move(values);
    }
    auto it2 = it->second.find(key);
    if ((it2 = it->second.find(key)) == it->second.end()) {
      it2 = it->second.emplace_hint(it2, key, value);
    } else {
      it2->second = value;
    }
  }

  /**
   * Get parameter for the current bar by name
   */
  template <typename T = string>
  T get(const string& key) const {
    return get<T>(section(), key);
  }

  /**
   * Get value of a variable by section and parameter name
   */
  template <typename T = string>
  T get(const string& section, const string& key) const {
    auto it = m_sections.find(section);
    if (it == m_sections.end()) {
      throw key_error("Missing section \"" + section + "\"");
    }
    if (it->second.find(key) == it->second.end()) {
      throw key_error("Missing parameter \"" + section + "." + key + "\"");
    }
    return dereference<T>(section, key, it->second.at(key));
  }

  /**
   * Get value of a variable by section and parameter name
   * with a default value in case the parameter isn't defined
   */
  template <typename T = string>
  T get(const string& section, const string& key, const T& default_value) const {
    try {
      string string_value{get<string>(section, key)};
      return dereference<T>(move(section), move(key), move(string_value));
    } catch (const key_error& err) {
      return default_value;
    } catch (const std::exception& err) {
      m_log.err("Invalid value for \"%s.%s\", using default value (reason: %s)", section, key, err.what());
      return default_value;
    }
  }

  /**
   * Get list of key-value pairs starting with a prefix by section.
   *
   * Eg: if you have in config `env-FOO = bar`,
   *    get_with_prefix(section, "env-") will return [{"FOO", "bar"}]
   */
  template <typename T = string>
  vector<pair<string, T>> get_with_prefix(const string& section, const string& key_prefix) const {
    auto it = m_sections.find(section);
    if (it == m_sections.end()) {
      throw key_error("Missing section \"" + section + "\"");
    }

    vector<pair<string, T>> list;
    for (const auto& kv_pair : it->second) {
      const auto& key = kv_pair.first;

      if (key.substr(0, key_prefix.size()) == key_prefix) {
        const T& val = get<T>(section, key);
        list.emplace_back(key.substr(key_prefix.size()), val);
      }
    }

    return list;
  }

  /**
   * Get list of values for the current bar by name
   */
  template <typename T = string>
  vector<T> get_list(const string& key) const {
    return get_list<T>(section(), key);
  }

  /**
   * Get list of values by section and parameter name
   */
  template <typename T = string>
  vector<T> get_list(const string& section, const string& key) const {
    vector<T> results;

    while (true) {
      try {
        string string_value{get<string>(section, key + "-" + to_string(results.size()))};
        T value{convert<T>(string{string_value})};

        if (!string_value.empty()) {
          results.emplace_back(dereference<T>(section, key, move(string_value)));
        } else {
          results.emplace_back(move(value));
        }
      } catch (const key_error& err) {
        break;
      }
    }

    if (results.empty()) {
      throw key_error("Missing parameter \"" + section + "." + key + "-0\"");
    }

    return results;
  }

  /**
   * Get list of values by section and parameter name
   * with a default list in case the list isn't defined
   */
  template <typename T = string>
  vector<T> get_list(const string& section, const string& key, const vector<T>& default_value) const {
    vector<T> results;

    while (true) {
      try {
        string string_value{get<string>(section, key + "-" + to_string(results.size()))};
        T value{convert<T>(string{string_value})};

        if (!string_value.empty()) {
          results.emplace_back(dereference<T>(section, key, move(string_value)));
        } else {
          results.emplace_back(move(value));
        }
      } catch (const key_error& err) {
        break;
      } catch (const std::exception& err) {
        m_log.err("Invalid value in list \"%s.%s\", using list as-is (reason: %s)", section, key, err.what());
        return default_value;
      }
    }

    if (!results.empty()) {
      return results;
      ;
    }

    return default_value;
  }

  void ignore_key(const string& section, const string& key) const;

  /**
   * Attempt to load value using the deprecated key name. If successful show a
   * warning message. If it fails load the value using the new key and given
   * fallback value
   */
  template <typename T = string>
  T deprecated(const string& section, const string& old, const string& newkey, const T& fallback) const {
    try {
      T value{get<T>(section, old)};
      warn_deprecated(section, old, newkey);
      return value;
    } catch (const key_error& err) {
      return get<T>(section, newkey, fallback);
    }
  }

  /**
   * @see deprecated<T>
   */
  template <typename T = string>
  T deprecated_list(const string& section, const string& old, const string& newkey, const vector<T>& fallback) const {
    try {
      vector<T> value{get_list<T>(section, old)};
      warn_deprecated(section, old, newkey);
      return value;
    } catch (const key_error& err) {
      return get_list<T>(section, newkey, fallback);
    }
  }

 protected:
  void copy_inherited();

  template <typename T>
  T convert(string&& value) const;

  /**
   * Dereference value reference and convert to template type
   */
  template <typename T>
  T dereference(const string& section, const string& key, const string& var) const {
    m_log.notice("deref(%s, %s, %s)", section, key, var);
    return convert<T>(dereference_string(section, key, var));
  }

  /**
   * Dereference value reference as a string
   */
  string dereference_string(const string& section, const string& key, const string& var) const {
    m_log.notice("deref_str(%s, %s, %s)", section, key, var);
    string result;
    size_t start = 0, end = 0;
    while (true) {
        // TODO: Allow escaping references, so that for example a literal '${}` is possible
        // Check if var contains a variable reference (format is `${...}`)
        start = var.find("${", end);
        if (start == string::npos) {
          break;
        }
        // Note that here `end` points to either the beginning of the string or
        // the first character after the last reference, so its position is before `start`!
        result += var.substr(end, start - end);

        end = var.find("}", start + 1);
        if (end == string::npos) {
          throw value_error("Unclosed reference at \"" + section + "." + key + "\" (missing closing `}`)");
        }

        // Strip out `${` and `}`
        string reference = var.substr(start + 2, end - start - 2);

        // Store result of dereferencing `reference` in `dereferenced`
        string dereferenced;
        size_t pos;
        if (reference.compare(0, 4, "env:") == 0) {
          dereferenced = dereference_env(reference.substr(4));
        } else if (reference.compare(0, 5, "xrdb:") == 0) {
          dereferenced = dereference_xrdb(reference.substr(5));
        } else if (reference.compare(0, 5, "file:") == 0) {
          dereferenced = dereference_file(reference.substr(5));
        } else if ((pos = reference.find(".")) != string::npos) {
          m_log.notice("deref_str: call to deref_local");
          dereferenced = dereference_local(reference.substr(0, pos), reference.substr(pos + 1), section);
        } else {
          throw value_error("Invalid reference defined at \"" + section + "." + key + "\"");
        }
        result += dereferenced;
        end++;
      }
    // Append remaining substring of `var` after last variable reference
    return result + var.substr(end);
  }

  /**
   * Dereference local value reference defined using:
   *  ${root.key}
   *  ${root.key:fallback}
   *  ${self.key}
   *  ${self.key:fallback}
   *  ${section.key}
   *  ${section.key:fallback}
   */
  string dereference_local(string section, const string& key, const string& current_section) const {
    if (section == "BAR") {
      m_log.warn("${BAR.key} is deprecated. Use ${root.key} instead");
    }

    section = string_util::replace(section, "BAR", this->section(), 0, 3);
    section = string_util::replace(section, "root", this->section(), 0, 4);
    section = string_util::replace(section, "self", current_section, 0, 4);

    try {
      // Try to look up the value that is being referenced
      m_log.notice("deref_local: call to get<string>");
      string string_value{get<string>(section, key)};

      m_log.notice("deref_local: call to deref-string(%s, %s)", section, key);
      return dereference_string(string(section), move(key), string_value);
    } catch (const key_error& err) {
      size_t pos;
      if ((pos = key.find(':')) != string::npos) {
        string fallback = key.substr(pos + 1);
        m_log.info("The reference ${%s.%s} does not exist, using defined fallback value \"%s\"", section,
            key.substr(0, pos), fallback);
        return fallback;
      }
      throw value_error("The reference ${" + section + "." + key + "} does not exist (no fallback set)");
    }
  }

  /**
   * Dereference environment variable reference defined using:
   *  ${env:key}
   *  ${env:key:fallback value}
   */
  string dereference_env(string var) const {
    size_t pos;
    string env_default;
    /*
     * This is needed because with only the string we cannot distinguish
     * between an empty string as default and not default
     */
    bool has_default = false;

    if ((pos = var.find(':')) != string::npos) {
      env_default = var.substr(pos + 1);
      has_default = true;
      var.erase(pos);
    }

    if (env_util::has(var)) {
      string env_value{env_util::get(var)};
      m_log.info("Environment var reference ${%s} found (value=%s)", var, env_value);
      return env_value;
    } else if (has_default) {
      m_log.info("Environment var ${%s} is undefined, using defined fallback value \"%s\"", var, env_default);
      return env_default;
    } else {
      throw value_error(sstream() << "Environment var ${" << var << "} does not exist (no fallback set)");
    }
  }

  /**
   * Dereference X resource db value defined using:
   *  ${xrdb:key}
   *  ${xrdb:key:fallback value}
   */
  string dereference_xrdb(string var) const {
    size_t pos;
#if not WITH_XRM
    m_log.warn("No built-in support to dereference ${xrdb:%s} references (requires `xcb-util-xrm`)", var);
    if ((pos = var.find(':')) != string::npos) {
      return var.substr(pos + 1);
    }
    return "";
#else
    if (!m_xrm) {
      throw application_error("xrm is not initialized");
    }

    string fallback;
    bool has_fallback = false;
    if ((pos = var.find(':')) != string::npos) {
      fallback = var.substr(pos + 1);
      has_fallback = true;
      var.erase(pos);
    }

    try {
      auto value = m_xrm->require<string>(var.c_str());
      m_log.info("Found matching X resource \"%s\" (value=%s)", var, value);
      return value;
    } catch (const xresource_error& err) {
      if (has_fallback) {
        m_log.info("%s, using defined fallback value \"%s\"", err.what(), fallback);
        return fallback;
      }
      throw value_error(sstream() << err.what() << " (no fallback set)");
    }
#endif
  }

  /**
   * Dereference file reference by reading its contents
   *  ${file:/absolute/file/path}
   *  ${file:/absolute/file/path:fallback value}
   */
  string dereference_file(string var) const {
    size_t pos;
    string fallback;
    bool has_fallback = false;
    if ((pos = var.find(':')) != string::npos) {
      fallback = var.substr(pos + 1);
      has_fallback = true;
      var.erase(pos);
    }
    var = file_util::expand(var);

    if (file_util::exists(var)) {
      m_log.info("File reference \"%s\" found", var);
      return string_util::trim(file_util::contents(var), '\n');
    } else if (has_fallback) {
      m_log.info("File reference \"%s\" not found, using defined fallback value \"%s\"", var, fallback);
      return fallback;
    } else {
      throw value_error(sstream() << "The file \"" << var << "\" does not exist (no fallback set)");
    }
  }

 private:
  const logger& m_log;
  string m_file;
  string m_barname;
  sectionmap_t m_sections{};

  /**
   * Absolute path of all files that were parsed in the process of parsing the
   * config (Path of the main config file also included)
   */
  file_list m_included;
#if WITH_XRM
  unique_ptr<xresource_manager> m_xrm;
#endif
};

POLYBAR_NS_END
