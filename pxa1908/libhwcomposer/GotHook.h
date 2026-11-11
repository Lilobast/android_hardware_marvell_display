/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __GOT_HOOK_H__
#define __GOT_HOOK_H__

#include <link.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Redirect an imported function of an already loaded library: rewrite the library's PLT (GOT)
 * slot for `sym` to `repl`. *orig receives the previous target. Only calls the library makes
 * through its PLT are affected. Returns 0 on success.
 */
struct got_hook_req {
    const char* lib;
    const char* sym;
    void* repl;
    void** orig;
    int result;
};

static int got_hook_cb(struct dl_phdr_info* info, size_t, void* data)
{
    struct got_hook_req* h = (struct got_hook_req*)data;
    if (info->dlpi_name == NULL || strstr(info->dlpi_name, h->lib) == NULL)
        return 0;

    ElfW(Addr) base = info->dlpi_addr;
    const ElfW(Dyn)* dyn = NULL;
    ElfW(Addr) relro_start = 0, relro_end = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn)*)(base + ph->p_vaddr);
        } else if (ph->p_type == PT_GNU_RELRO) {
            relro_start = base + ph->p_vaddr;
            relro_end = relro_start + ph->p_memsz;
        }
    }
    if (dyn == NULL)
        return 1;

    const ElfW(Sym)* symtab = NULL;
    const char* strtab = NULL;
    const ElfW(Rel)* jmprel = NULL;
    size_t jmprel_size = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_SYMTAB:   symtab = (const ElfW(Sym)*)(base + dyn->d_un.d_ptr); break;
        case DT_STRTAB:   strtab = (const char*)(base + dyn->d_un.d_ptr); break;
        case DT_JMPREL:   jmprel = (const ElfW(Rel)*)(base + dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: jmprel_size = dyn->d_un.d_val; break;
        }
    }
    if (symtab == NULL || strtab == NULL || jmprel == NULL)
        return 1;

    long page = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < jmprel_size / sizeof(ElfW(Rel)); i++) {
        if (strcmp(strtab + symtab[ELF32_R_SYM(jmprel[i].r_info)].st_name, h->sym) != 0)
            continue;
        void** slot = (void**)(base + jmprel[i].r_offset);
        void* pg = (void*)((ElfW(Addr))slot & ~(page - 1));
        bool in_relro = (ElfW(Addr))slot >= relro_start && (ElfW(Addr))slot < relro_end;
        if (in_relro && mprotect(pg, page, PROT_READ | PROT_WRITE) != 0)
            return 1;
        if (h->orig)
            *h->orig = *slot;
        *slot = h->repl;
        if (in_relro)
            mprotect(pg, page, PROT_READ);
        h->result = 0;
        return 1;
    }
    return 1;
}

static inline int got_hook(const char* lib, const char* sym, void* repl, void** orig)
{
    struct got_hook_req h = { lib, sym, repl, orig, -1 };
    dl_iterate_phdr(got_hook_cb, &h);
    return h.result;
}

#endif
