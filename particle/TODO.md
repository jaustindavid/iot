# Issues found, not resolved

1. "extra" was allowed in a script, but breaks the codegen, causes a typo
  at 492: ```        if (!matched && (_in_vec(tk_extra, a.pos))) {```

2. codegen not respecting extras (may have been caused by #1?)
```
src/CoatiEngine.cpp:324:47: error: 'extras' was not declared in this scope; did you mean 'tk_extras'?
  324 |                     std::vector<Point> _cells(extras, extras + extras_count);
      |                                               ^~~~~~
      |                                               tk_extras
```

this line should have been `Point _dest = _find_closest(a.pos, tk_extras);`


3. ladybug despawns ladybugs; propose `despawn neighbors where extras and role=0`

4. aphids: idle color no worky?  missing: are still pretty bright?  spawned have no color?  setting a color for role == 0 overrides everything else?

5. slow physics also breaks interpolation rate

6. sim doesn't work with 16x16

7. tortoise / ladybug: wander ON a lit area (new primitive "current" ?)

8. remove wobbly params from the script.
