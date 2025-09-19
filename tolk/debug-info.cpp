#include "tolk.h"
#include <ast.h>
#include <compiler-state.h>
#include "ast-stringifier.h"

namespace tolk {

void insert_debug_info_inner(SrcLocation loc, ASTNodeKind kind, CodeBlob& code, size_t line_offset, std::string descr) {
  if (!G.settings.with_debug_info) {
    return;
  }

  if (kind == ast_artificial_aux_vertex || kind == ast_throw_statement) {
    return;
  }

#ifdef TOLK_DEBUG
  const auto last_op = *std::find_if(code._vector_of_ops.rbegin(), code._vector_of_ops.rend(), [](const auto& it) {
    return it->cl != Op::_DebugInfo;
  });
#endif

  auto& op = code.emplace_back(loc, Op::_DebugInfo);
  op.source_map_entry_idx = G.source_map.size();

  auto info = SourceMapEntry{};
  info.idx = op.source_map_entry_idx;
  info.descr = descr;
  info.is_entry = kind == ast_function_declaration;

#ifdef TOLK_DEBUG
  if (last_op) {
    std::stringstream st;
    last_op->show(st, code.vars, "", 4);

    info.opcode = st.str();
  }
#endif
  info.ast_kind = ASTStringifier::ast_node_kind_to_string(kind);

  if (const SrcFile* src_file = loc.get_src_file(); src_file != nullptr) {
    const auto& pos = src_file->convert_offset(loc.get_char_offset());

    info.loc.file = src_file->realpath;
    info.loc.offset = loc.get_char_offset();
    info.loc.line = pos.line_no;
    info.loc.line_offset = line_offset;
    info.loc.col = pos.char_no - 1;
    info.loc.length = pos.line_str.length();
  }

  info.func_name = code.fun_ref->name;
  if (code.name != info.func_name) {
    info.inlined_to_func_name = code.name;
  }
  info.func_inline_mode = code.fun_ref->inline_mode;
  G.source_map.push_back(info);
}

void insert_debug_info(AnyV v, CodeBlob& code) {
  insert_debug_info_inner(v->loc, v->kind, code, 0, "");
}

}
