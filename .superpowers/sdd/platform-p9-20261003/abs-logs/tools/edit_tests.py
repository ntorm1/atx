"""ABS: the AbsentMembers gtests (abs_tests.cpp.txt) at the end of strategy_ic_runner_test.cpp and
the `gate` field column they read."""
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
from abs_edit import File  # noqa: E402

t = File("C:/atx-wt/pool-28/atx-impl/tests/strategy_ic_runner_test.cpp")
t.once("""std::vector<f64> field_column(const std::string& name,f64 scale) {
  std::vector<f64> out(D*N);
  for (usize d=0;d<D;++d) for (usize i=0;i<N;++i)
    out[d*N+i]=name=="grp_a"?group_label(d,i):(name=="mkt_ret"?market_value(d):
        (name=="iv_atm_21d"?.3+.01*static_cast<f64>(i):si_value(d,i,scale)));
  return out;
""", """// `gate` (P9 lane ABS): the scale on every cell (1 keeps a gated member's data, NaN removes it).
std::vector<f64> field_column(const std::string& name,f64 scale) {
  std::vector<f64> out(D*N);
  for (usize d=0;d<D;++d) for (usize i=0;i<N;++i)
    out[d*N+i]=name=="grp_a"?group_label(d,i):(name=="mkt_ret"?market_value(d):
        (name=="iv_atm_21d"?.3+.01*static_cast<f64>(i):
        (name=="gate"?scale:si_value(d,i,scale))));
  return out;
""")
block = (HERE / "abs_tests.cpp.txt").read_text()
assert t.text.endswith("}\n} // namespace\n")
t.text = t.text[: -len("} // namespace\n")] + block + "} // namespace\n"
t.save()
print("ok")
