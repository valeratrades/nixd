{
  options = {
    count = {
      descr = "signed integer";
      check = builtins.isInt;
    };
    outputs = {
      options = {
        buffer = {
          descr = "signed integer";
          check = builtins.isInt;
        };
        name = {
          descr = "string";
          check = builtins.isString;
        };
      };
    };
  };
}
