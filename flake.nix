{
  inputs = {
  };
  outputs = { self }:
  {
    packages.x86_64-linux = let
      legacy = import ./. {};
    in {
      vc4-stage2 = legacy.vc4.vc4.stage2;
    };
  };
}