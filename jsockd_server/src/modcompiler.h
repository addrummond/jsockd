#ifndef MODCOMPILER_H_
#define MODCOMPILER_H_

int compile_module_file(const char *module_filename,
                        const char *privkey_filename,
                        const char *output_filename, const char *version,
                        int qjsc_strip_flags);
int output_key_file(const char *key_file);

#endif
