#include "tolk.h"
#include "pipeline.h"
#include "compiler-state.h"
#include "type-system.h"
#include "td/utils/JsonBuilder.h"
#include <fstream>

namespace tolk {

void pipeline_process_debug_info(std::ostream& debug_out) {
  if (!G.settings.with_debug_info) {
    return;
  }


  td::JsonBuilder _jb;
  auto objb = _jb.enter_object();

  {
    td::JsonBuilder jsonb;
    auto arrb = jsonb.enter_array();
    for (auto glob_var : G.all_global_vars) {
      auto vb = arrb.enter_value();
      auto ob = vb.enter_object();

      ob("name", glob_var->name);
      ob("type", glob_var->declared_type->as_human_readable());
    }
    arrb.leave();

    objb("globals", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  {
    td::JsonBuilder jsonb;
    auto arrb = jsonb.enter_array();

    for (size_t i = 0; i < G.source_map.size(); ++i) {
      const auto &entry = G.source_map[i];
      auto vb = arrb.enter_value();
      auto ob = vb.enter_object();
      ob("idx", td::JsonRaw(std::to_string(entry.idx)));

      if (entry.is_entry) {
        ob("is_entry", td::JsonBool(entry.is_entry));
      }

#ifdef TOLK_DEBUG
      if (i + 1 < G.source_map.size()) {
        ob("opcode", G.source_map[i + 1].opcode);
      }
#endif
      ob("ast_kind", entry.ast_kind);

      // Used only for source map debug
      if (const auto file = G.all_src_files.find_file(entry.loc.file)) {
        int start_offset = -1;
        int end_offset = -1;
        int cur_line = 0;
        long search_line = entry.loc.line;

        for (size_t ch_idx = 0; ch_idx < file->text.length(); ++ch_idx) {
          const auto &ch = file->text[ch_idx];
          if (ch == '\n') {
            cur_line++;

            if (cur_line == search_line - 1) {
              start_offset = static_cast<int>(ch_idx + 1);
            }

            if (cur_line == search_line && start_offset != -1) {
              end_offset = static_cast<int>(ch_idx);
              break;
            }
          }
        }

        const std::string line = file->text.substr(start_offset, end_offset - start_offset);

        // const auto& pos = file->convert_offset(entry.loc.offset);
        // std::string line = std::string(pos.line_str);
        ob("line_str", line);

        std::string underline = "";
        for (int j = 0; j < entry.loc.col; ++j) {
          underline += " ";
        }
        underline += "^";

        ob("line_off", underline);
      }

      ob("file", entry.loc.file);
      ob("line", static_cast<td::int64>(entry.loc.line));
      ob("pos", static_cast<td::int64>(entry.loc.col));
      ob("line_offset", static_cast<td::int64>(entry.loc.line_offset));
      ob("length", static_cast<td::int64>(entry.loc.length));

      td::JsonBuilder varb;
      auto vararrb = varb.enter_array();
      for (const auto &[var, value] : entry.vars) {
        auto varb2 = vararrb.enter_value();
        auto varbo = varb2.enter_object();
        varbo("name", var.name.empty() ? "'" + std::to_string(var.ir_idx) : var.name);
        varbo("type", var.v_type == nullptr ? "" : var.v_type->as_human_readable());

        if (var.parent_type != nullptr) {
          auto union_parent = var.parent_type->try_as<TypeDataUnion>();
          if (union_parent != nullptr) {
            td::JsonBuilder parent_type_builder;
            auto parent_type_array_builder = parent_type_builder.enter_array();

            for (auto variant : union_parent->variants) {
              auto array_value = parent_type_array_builder.enter_value();
              array_value << variant->as_human_readable();
            }

            parent_type_array_builder.leave();
            varbo("possible_qualifier_types", td::JsonRaw(parent_type_builder.string_builder().as_cslice()));
          }
        }

        // varbo("parent_type", var.parent_type == nullptr ? "" : var.parent_type->as_human_readable());
        if (!value.empty()) {
          varbo("value", value);
        }
      }
      vararrb.leave();

      td::JsonRaw vararrs(varb.string_builder().as_cslice());

      ob("vars", vararrs);
      ob("func", entry.func_name);
      ob("func_inline_mode", static_cast<td::int64>(entry.func_inline_mode));
      if (entry.before_inlined_function_call) {
        ob("before_inlined_function_call", td::JsonBool(entry.before_inlined_function_call));
      }
      if (entry.after_inlined_function_call) {
        ob("after_inlined_function_call", td::JsonBool(entry.after_inlined_function_call));
      }
    }
    arrb.leave();

    objb("locations", td::JsonRaw(jsonb.string_builder().as_cslice()));
  }

  objb.leave();

  debug_out << _jb.string_builder().as_cslice().str();
}

}  // namespace tolk
