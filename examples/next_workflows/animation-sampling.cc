// SPDX-License-Identifier: Apache-2.0
#include "next/lightusd-next.hh"
#include "next/writer/value-printer.hh"
#include <iostream>

int main(int argc, char** argv) {
  namespace usd = lightusd::next;
  if (argc != 3) { std::cerr << "Usage: animation_sampling FILE /Prim.attribute\n"; return 2; }
  usd::Stage stage;
  std::string warning, error;
  if (!usd::LoadUSDComposed(argv[1], &stage, &warning, &error)) { std::cerr << error << '\n'; return 1; }
  usd::AssetResolver resolver;
  const std::string root = resolver.ResolvePath(argv[1]);
  usd::EvalOptions options;
  options.clip_stage_cache = std::make_shared<usd::ValueClipStageCache>();
  options.clip_stage_loader = [&](const std::string& path, usd::Stage* clip, std::string* w, std::string* e) {
    return usd::LoadUSDComposed(resolver.ResolvePath(path, root), clip, w, e);
  };
  const usd::Path path(argv[2]);
  const auto prim = stage.GetPrimAtPath(path.prim_path());
  if (!prim || path.property_name().empty()) return 2;
  usd::AttributeEval eval(&stage);
  for (auto mode : {usd::TimeInterpolation::Held, usd::TimeInterpolation::Linear}) {
    options.interp = mode;
    for (const auto time : {usd::TimeQuery::Default(), usd::TimeQuery::Numeric(0),
                            usd::TimeQuery::Numeric(0.5), usd::TimeQuery::Numeric(1)}) {
      options.time = time;
      const auto result = eval.EvalWith(prim, path.property_name(), options);
      if (!result.success) { std::cerr << result.error << '\n'; return 1; }
      std::cout << (mode == usd::TimeInterpolation::Held ? "held " : "linear ")
                << (time.is_default() ? "default" : std::to_string(time.numeric_time()))
                << " = " << usd::PrintValue(result.value) << '\n';
    }
  }
  stage.Traverse([&](const usd::UsdPrim& p) {
    if (usd::IsSkelAnimation(p)) {
      usd::SkelAnimationData data;
      if (usd::GetSkelAnimationDataAtTime(stage, p, &data, 0.5))
        std::cout << "skeleton sample " << p.GetPath().str() << " joints=" << data.joints.size() << '\n';
    }
    return true;
  });
  return 0;
}
