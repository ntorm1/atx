// Ingest of stage receipts (`atx.stage-receipt/v1`; <state dir>/receipts/<NN>-<stage>[.failed-k]
// .json). Writer: atx-engine/tools/stage_chain.py Chain.write (sorted keys, two-space indent,
// LF): rule py-indent2-sorted, so no key order is stored; a failed receipt's error / code (and
// any other key) are kept in extra.

#include <string>
#include <utility>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/engine/research/store/catalog/ops_records.hpp"
#include "atx/engine/research/store/catalog/seal_guard.hpp"
#include "research/store/catalog/catalog_detail.hpp"

namespace atx::engine::research::store::catalog::detail {

void ingest_stage_receipt(const FamilyInput &in, IngestResult &out) {
  const std::string receipts_dir = parent_of(in.file.path);
  const std::string state_dir = parent_of(receipts_dir);
  if (!is_relpath(state_dir) || leaf_of(receipts_dir) != "receipts") {
    out.unparsed = true;
    return;
  }
  DocFields f{in.doc};
  StageReceiptRow row;
  row.state_dir = state_dir;
  row.file_name = leaf_of(in.file.path);
  row.schema = f.req_text("schema");
  row.chain = f.req_text("chain");
  row.stage = f.req_text("stage");
  row.stage_index = f.req_integer("index");
  row.status = f.req_text("status");
  row.inputs = f.req_json("inputs");
  row.outputs = f.req_json("outputs");
  row.started_utc = f.req_text("started_utc");
  row.seconds = f.req_real("seconds");
  if (!f.shape_ok()) {
    out.unparsed = true;
    return;
  }
  row.extra = f.extra();
  row.file_sha256 = in.file.sha256;
  out.writes.emplace_back([row = std::move(row)](core::db::Database &db) -> core::Status {
    return upsert(db, row);
  });
}

} // namespace atx::engine::research::store::catalog::detail
