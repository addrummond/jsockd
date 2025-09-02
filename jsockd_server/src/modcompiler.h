#ifndef MODCOMPILER_H_
#define MODCOMPILER_H_

int compile_module_file(const char *module_filename,
                        const char *key_file_prefix,
                        const char *output_filename);
int output_key_file(const char *key_file);

#endif
