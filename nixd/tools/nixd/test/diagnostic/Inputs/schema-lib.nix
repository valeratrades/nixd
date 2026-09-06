let
  dot = loc: builtins.concatStringsSep "." loc;

  docList =
    prefix: opts:
    builtins.concatMap (
      n:
      let
        o = opts.${n};
        loc = prefix ++ [ n ];
      in
      [ { inherit loc; } ] ++ (if o ? options then docList loc o.options else [ ])
    ) (builtins.attrNames opts);

  mkConfig =
    prefix: opts: values:
    builtins.mapAttrs (
      n: o:
      let
        loc = prefix ++ [ n ];
      in
      if o ? options then
        mkConfig loc o.options (if values ? ${n} then values.${n} else { })
      else if !(values ? ${n}) then
        throw "The option `${dot loc}' was accessed but has no value defined."
      else if o.check values.${n} then
        values.${n}
      else
        throw "A definition for option `${dot loc}' is not of type `${o.descr}'."
    ) opts;
in
{
  lib = {
    modules.setDefaultModuleLocation = _: m: m;
    getAttrFromPath = path: attrs: builtins.foldl' (a: n: a.${n}) attrs path;
    optionAttrSetToDocList = opts: docList [ ] opts;
    evalModules =
      { modules }:
      let
        decl = (import (builtins.head modules)).options;
        cfg = if builtins.length modules > 1 then builtins.elemAt modules 1 else { };
      in
      {
        options = decl;
        config = mkConfig [ ] decl cfg;
      };
  };
}
