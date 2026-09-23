#include "managed_json.h"
#include <cassert>
#include <iostream>
using nlohmann::json;
bool rejected(const std::string &s) { try { managed_json::parse_object_keys(s); return false; } catch (...) { return true; } }
int main() {
  const std::string prepare=R"({"op":"desktop.prepare","lease":{"lease_id":"fixture","lease_epoch":1},"identity":{"lease_id":"fixture","lease_epoch":1},"profile":{"output_mode_pixels":{"width":2560,"height":1654},"logical_size":{"width":1280,"height":827},"stream_pixels":{"width":2486,"height":1608},"output_scale":2,"fps":30,"bitrate_kbps":8000}})";
  assert(managed_json::parse_object_keys(prepare)==json::parse(prepare));
  assert(managed_json::parse_object_keys(R"({"rows":[{"width":1},{"width":2}],"width":3})")["width"]==3);
  assert(rejected(R"({"op":"desktop.status","op":"desktop.prepare"})"));
  assert(rejected(R"({"profile":{"width":1,"width":2}})"));
  assert(rejected(R"({"rows":[{"width":1,"width":2}]})"));
  assert(rejected(R"({"a":{"x":1},"a":{"x":2}})"));
  assert(rejected(R"({"a":{"x":1},"b":[{"y":1}],"a":2})"));
  assert(rejected("{"));
  std::cout<<"ordinary_nested_json_contract: PASS\n";
}
