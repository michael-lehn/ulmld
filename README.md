# ulmld

Currently the linker is not integrated into ULM-generator. So you have to
configure the linker manually.

Usually it just requires to edit the file `path-to-ulm`. For example, if this
file contains the line

```
${HOME}/hpc0/ulm-generator/1_ulm_build/stack/
```

the linker will be configured to work for the ULM architecture in this
directory. Check that the specified path contains a `ulm` virtual machine built
by the ULM-generator.

Note that shell variables in this path must be inside of `${}`. 
