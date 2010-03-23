#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <signal.h>

#include "cpu.h"
#include "argos-config.h"
#include "argos-common.h"
#include "argos-check.h"
#include "../exec-all.h"
#include "libdasm/libdasm.h"
#include "win2k/winternl.h"
#include "argos-tracksc.h"

target_phys_addr_t _phys_addr(CPUX86State *env, target_ulong addr)
{
    target_phys_addr_t paddr =
        cpu_get_phys_page_debug(env, addr + env->segs[R_CS].base);
    if (paddr != -1)
    {
        PhysPageDesc *pdesc = phys_page_find(paddr >> TARGET_PAGE_BITS);
        unsigned long pd;
        if (!pdesc) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = pdesc->phys_offset;
        }
        unsigned long phys_addr = (unsigned long)phys_ram_base
            + (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);

        return phys_addr;
    }
    else
    {
        return 0;
    }
}
#define VALID_PTR(X) \
     ( (X) != 0 )

#define PHYS_ADDR(X) \
    _phys_addr(env, (X))

static INSTRUCTION current_instr;
// The pc must be a host virtual address.
static int _get_instr_len(unsigned long pc);
static void _log_instr(argos_shellcode_context_t * context);
static void _get_imported_modules(CPUX86State * env);
static target_ulong _get_current_thread_id(CPUX86State * env);
static unsigned char _in_shellcode_context(CPUX86State * env);
static unsigned char _is_tracking(CPUX86State * env);
static void _save_shellcode_context(CPUX86State * env);
static void _start_analysis_phase(CPUX86State * env);
static void _start_tracking_phase(CPUX86State * env);
extern void vm_stop(int reason);

void argos_tracksc_init(CPUX86State * env)
{
    const unsigned filename_size = 128;
    char filename[filename_size];
    // Initialize the structure that holds the context of the shell-code
    // being tracked.
    memset(&env->shellcode_context, 0, sizeof(argos_shellcode_context_t));

    env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_IDLE;
    // Set the default condition when to stop executing and tracking
    // shell-code.
    env->shellcode_context.stop_condition = SSC_FIRST_SYSTEM_CALL;

    snprintf(filename, filename_size,  LOG_SC_FL_TEMPLATE,
            argos_instance_id);
    env->shellcode_context.logfile = fopen(filename, "wb");
    if (!env->shellcode_context.logfile) 
    {
        argos_logf("Failed to open the shellcode log file %s.\n",
                filename);
        exit(1);
    }
    else
    {
        argos_logf("Opened shellcode log file %s.\n", filename);
    }
}

void argos_tracksc_stop(CPUX86State * env)
{
    if (env->shellcode_context.logfile)
    {
        fclose(env->shellcode_context.logfile);
        argos_logf("Closed shellcode log file\n");
        env->shellcode_context.logfile = 0;
        env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_IDLE;
    }

    // Clean-up existing imported modules information.
    argos_tracksc_imported_modules * delete_cursor, * next;
    next = env->shellcode_context.imported_modules;
    while ( next )
    {
        delete_cursor = next;
        next = delete_cursor->next;

        if ( delete_cursor->module )
            free(delete_cursor->module);

        free(delete_cursor);
    }
}

unsigned char argos_tracksc_is_active( CPUX86State * env)
{
    return env->shellcode_context.phase != ARGOS_TRACKSC_PHASE_IDLE;
}

void argos_tracksc_enable(CPUX86State * env)
{
    argos_logf("Starting shell-code tracking...\n");

    // Save context of the shellcode.
    _save_shellcode_context(env);

    // We are now entering the analysis phase.
    _start_analysis_phase(env);

    // Now shift to tracking phase.
    _start_tracking_phase(env);
}

void argos_tracksc_store_context(CPUX86State * env)
{
    // Here we are saving context of the instructions that are 
    // executed during the tracking of shell-code.
    // Everything that is logged is before the execution of the 
    // actual instruction.
    // Check if we are executing a tainted instruction.
    // This assumes that the shell-code is marked tainted by Argos.
    if ( _is_tracking(env)
            && _in_shellcode_context(env)
            && argos_dest_pc_isdirty(env, env->eip)
            && env->shellcode_context.executed_eip != env->eip)
    {
#ifdef ARGOS_NET_TRACKER
        unsigned i;
        unsigned max_stage = 0;
#endif // ARGOS_NET_TRACKER

        // hp = host physical, gv = guest virtual, 
        // and correspond to a memory address.
        unsigned long hp_pc = 0, gv_pc = 0;

        gv_pc = env->segs[R_CS].base + env->eip;
        // Casting the phys_ram_base to target_ulong will
        // trim the address from 64 bits to 32 bits and yields
        // a wrong address when executing Argos on an amd64 host.
        hp_pc = get_phys_addr_code(env, gv_pc) +
            (unsigned long)phys_ram_base;

        //env->shellcode_context.instruction_size = env->current_tb->size;
        env->shellcode_context.instruction_size =
            _get_instr_len(hp_pc);

        /*if ( env->current_tb->size !=
          env->shellcode_context.instruction_size )
          {
          argos_logf("!!! Eip offset detected !!!\n");
          }*/

        memcpy(env->shellcode_context.instruction, (void *)hp_pc,
                env->shellcode_context.instruction_size);

        env->shellcode_context.executed_eip = env->eip;
        env->shellcode_context.logged = 1;

#ifdef ARGOS_NET_TRACKER
        memset(env->shellcode_context.instruction_netidx, 0,
                MAX_INSTRUCTION_SIZE);

        for ( i = 0; i < env->shellcode_context.instruction_size;
                i++ )
        {
            argos_netidx_t* netidx = ARGOS_NETIDXPTR(hp_pc + i);
            if (netidx != 0)
            {
                // Currently the stage of the instruction equals 
                // to the highest stage of an individual 
                // instruction byte.
                if ( ARGOS_GET_STAGE(*netidx) > max_stage )
                {
                    max_stage = ARGOS_GET_STAGE(*netidx);
                }
                env->shellcode_context.instruction_netidx[i] =
                    *netidx;
            }
            else
            {
                env->shellcode_context.instruction_netidx[i] = 0;
            }
        }
        env->shellcode_context.instruction_stage = max_stage;
#endif // ARGOS_NET_TRACKER
    }
}

void argos_tracksc_log_instruction(CPUX86State * env)
{
    // Here we are going to log the instructions that belong to the 
    // shellcode.
    // This is after the execution of the instruction so the eip register 
    // points to the next instruction.
    if (env->shellcode_context.logged)
    {
        unsigned i;

#ifdef ARGOS_NET_TRACKER
        // Check if we are executing an instruction at a new stage.
        if ( env->shellcode_context.trace_stage <
                env->shellcode_context.instruction_stage )
        {
            fprintf(env->shellcode_context.logfile, "Stage %u -> %u\n", 
                    env->shellcode_context.trace_stage,
                    env->shellcode_context.instruction_stage);

            env->shellcode_context.trace_stage =
                env->shellcode_context.instruction_stage;
        }
#endif // ARGOS_NET_TRACKER

        // Print the eip address of the current executing instruction.
        fprintf(env->shellcode_context.logfile, "0x%x ", 
                env->shellcode_context.executed_eip);

        fprintf(env->shellcode_context.logfile, "\t");

        _log_instr(&env->shellcode_context);

        fprintf(env->shellcode_context.logfile, "\t");
        fprintf(env->shellcode_context.logfile, " ");

        // Print the bytes of the instruction
        for (i = 0; i < env->shellcode_context.instruction_size; i++)
        {
            fprintf(env->shellcode_context.logfile, "%x ",
                    env->shellcode_context.instruction[i] & 0xFF);
        }
        fprintf(env->shellcode_context.logfile, "\t");

#ifdef ARGOS_NET_TRACKER
        // Print the netidx's of the instruction bytes.
        for (i = 0; i < env->shellcode_context.instruction_size; i++)
        {
            fprintf(env->shellcode_context.logfile, "[%u] ",
                    ARGOS_GET_NETIDX(
                        env->shellcode_context.instruction_netidx[i])
                   );
        }

        fprintf(env->shellcode_context.logfile, "\t");
#endif // ARGOS_NET_TRACKER

        // If the instruction performed a load,
        // print the address of the load and the value loaded.
        if (env->shellcode_context.loadedby_eip ==
                env->shellcode_context.executed_eip)
        {

            fprintf(env->shellcode_context.logfile, " <- @0x%lx (0x%x) ",
                    env->shellcode_context.load_addr,
                    env->shellcode_context.load_value);

            fprintf(env->shellcode_context.logfile, "\t");

#ifdef ARGOS_NET_TRACKER
            // Print the netidx's belonging to the value loaded.
            for (i = 0; i < env->shellcode_context.load_size; i++)
            {
                argos_netidx_t* netidx;
                // TODO: check if address is guest virtual or 
                // guest physical address!
                // Not all cases are checked, which can result in an
                // invalid netidx.
                if (env->shellcode_context.load_addr_type ==
                        ARGOS_HOST_VIRTUAL_ADDR)
                {
                    netidx = ARGOS_NETIDXPTR(
                            env->shellcode_context.load_addr + i);
                }
                else
                {
                    if (env->shellcode_context.load_value_netidx != 0)
                    {
                        netidx =
                            env->shellcode_context.load_value_netidx + i;
                    }
                    else
                    {
                        netidx = 0;
                    }
                }

                if (netidx != 0)
                {
                    // We cannot always retrieve the address fo the guest,
                    // so we annotate the address to show that it a
                    // host virtual address.
                    if (env->shellcode_context.load_addr_type ==
                            ARGOS_HOST_VIRTUAL_ADDR)
                    {
                        fprintf(env->shellcode_context.logfile, "@[%u] ",
                                ARGOS_GET_NETIDX(*netidx));
                    }
                    else
                    {
                        fprintf(env->shellcode_context.logfile, "[%u] ",
                                ARGOS_GET_NETIDX(*netidx));
                    }
                }
                else
                {
                    fprintf(env->shellcode_context.logfile, "[%u] ", 0);
                }
            }
#endif // ARGOS_NET_TRACKER
            // Reset the load context.
            env->shellcode_context.load_addr = 0;
            env->shellcode_context.load_value = 0;
            env->shellcode_context.load_size = 0;
            env->shellcode_context.loadedby_eip = 0;
        }

        // If the instruction performed a store, print the store address
        // and value.
        if (env->shellcode_context.storedby_eip ==
                env->shellcode_context.executed_eip)
        {
            fprintf(env->shellcode_context.logfile, " -> @0x%lx (0x%x) ",
                    env->shellcode_context.store_addr,
                    env->shellcode_context.store_value);

            fprintf(env->shellcode_context.logfile, "\t");

#ifdef ARGOS_NET_TRACKER
            for (i = 0; i < env->shellcode_context.store_size; i++)
            {
                argos_netidx_t* netidx;

                if (env->shellcode_context.store_addr_type == 
                        ARGOS_HOST_VIRTUAL_ADDR)
                {
                    netidx = ARGOS_NETIDXPTR(
                            env->shellcode_context.store_addr + i);
                }
                else
                {
                    if (env->shellcode_context.store_value_netidx != 0)
                    {
                        netidx =
                            env->shellcode_context.store_value_netidx + i;
                    }
                    else
                    {
                        netidx = 0;
                    }
                }

                if (netidx != 0)
                {
                    if (env->shellcode_context.store_addr_type ==
                            ARGOS_HOST_VIRTUAL_ADDR)
                    {
                        fprintf(env->shellcode_context.logfile, "@[%u] ",
                                ARGOS_GET_NETIDX(*netidx));
                    }
                    else
                    {
                        fprintf(env->shellcode_context.logfile, "[%u] ",
                                ARGOS_GET_NETIDX(*netidx));
                    }
                }
                else
                {
                    fprintf(env->shellcode_context.logfile, "[%u] ", 0);
                }
            }
#endif // ARGOS_NET_TRACKER
            // Reset the store context.
            env->shellcode_context.store_addr = 0;
            env->shellcode_context.store_value = 0;
            env->shellcode_context.store_size = 0;
            env->shellcode_context.storedby_eip = 0;
        }
        fprintf(env->shellcode_context.logfile, "\n");
        fflush(env->shellcode_context.logfile);

        // We have logged the code.
        env->shellcode_context.logged = 0;

        // Increase the number of executed instruction and check the
        // stop condition.
        env->shellcode_context.instruction_cnt++;
        if (env->shellcode_context.stop_condition == SSC_MAX_INSTR_CNT
                && env->shellcode_context.instruction_cnt ==
                MAX_INSTRUCTION_CNT )
        {
            env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_IDLE;
            // For now we just pause the vm.
            vm_stop(0);
        }
    }
}

void argos_tracksc_check_for_invalid_system_call(CPUX86State * env)
{
    if ( _is_tracking(env) && env->shellcode_context.is_system_call)
    {
        // Check the shell-code tracking stop condition.
        if ( env->shellcode_context.stop_condition ==
                SSC_FIRST_SYSTEM_CALL )
        {
            env->shellcode_context.is_system_call = 0;
            env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_IDLE;
            // The system call number resides in the register eax for the
            // int 0x2e as well for the sysenter system call mechanism.
            fprintf(env->shellcode_context.logfile,
                    "Prevented shellcode from calling system call 0x%x.\n",
                    env->regs[R_EAX]);

            // For now we just pause the vm.
            vm_stop(0);
        }
    }
}

int argos_tracksc_is_valid_system_call(CPUX86State * env)
{
    // If we are in the correct context.
    if (_is_tracking(env) && _in_shellcode_context(env))
    {
        argos_logf("The shellcode is trying to make a system call.\n");
        // Flag that a system call is pending.
        env->shellcode_context.is_system_call = 1;
        return 0;
    }
    else
    {
        return 1;
    }
}

int argos_tracksc_logged_invalid_system_call(CPUX86State * env)
{
    if ( _is_tracking(env) )
    {
        return env->shellcode_context.is_system_call;
    }
    else
    {
        return 0;
    }
}

void argos_tracksc_check_function_call( CPUX86State * env)
{
    sigset_t alrmset;

    if (sigprocmask(SIG_BLOCK, &alrmset, NULL) != 0)
        perror("could not temporarily block signals");

    if ( _is_tracking(env) && _in_shellcode_context(env) )
    {
        if ( !argos_dest_pc_isdirty(env, env->eip))
        {
            if (  env->shellcode_context.imported_modules )
            {
                // Filter calls done in kernel-mode.
                // TODO: Have to check for runtime flags /3GB and
                // /USERVA, because they allow for a greater private
                // addressing space at the expense of a smaller kernel 
                // addressing space.
                if ( env->eip < 0x80000000 )
                {
                    argos_tracksc_imported_modules * cursor =
                        env->shellcode_context.imported_modules;
                    while ( cursor != NULL )
                    {
                        if ( cursor->module )
                        {
                            if ( env->eip > cursor->module->begin_address
                                    && env->eip < cursor->module->end_address)
                            {
                                target_ulong function_address_table =
                                    cursor->module->begin_address +
                                    cursor->module->function_address_table_rva;

                                target_ulong function_ordinal_table =
                                    cursor->module->begin_address +
                                    cursor->module->function_ordinal_table_rva;

                                target_ulong function_name_table =
                                    cursor->module->begin_address +
                                    cursor->module->function_name_table_rva;

                                if ( !VALID_PTR(function_name_table) )
                                {
                                    argos_logf("Invalid pointer to function name table!!!\n");
                                    goto exit_call_check;
                                }

                                unsigned i,j;
                                for (i = 0; i < cursor->module->number_of_functions; ++i)
                                {
                                    target_ulong * function_address =
                                        (target_ulong *)PHYS_ADDR(function_address_table + i * sizeof(target_ulong));

                                    if ( !VALID_PTR(function_address) )
                                    {
                                        argos_logf("Invalid function address pointer!!!\n");
                                        goto exit_call_check;
                                    }

                                    if ( env->eip == (*function_address + cursor->module->begin_address) )
                                    {
                                        for (j = 0; j < cursor->module->number_of_functions; ++j)
                                        {
                                            unsigned short * function_ordinal = (unsigned short *)
                                                PHYS_ADDR(function_ordinal_table + j * sizeof(unsigned short));

                                            if ( !VALID_PTR(function_ordinal) )
                                            {
                                                argos_logf("Invalid function ordinal pointer!!!\n");
                                                goto exit_call_check;
                                            }

                                            if ( i == *function_ordinal )
                                            {
                                                if ( j < cursor->module->number_of_functions_with_names )
                                                {
                                                    target_ulong * function_name_rva =
                                                        (target_ulong *)PHYS_ADDR(function_name_table + j * sizeof(target_ulong));

                                                    if ( !VALID_PTR(function_name_rva) )
                                                    {
                                                        argos_logf("Invalid function name rva pointer!!!\n");
                                                        goto exit_call_check;
                                                    }

                                                    const char * function_name =
                                                        (const char *)PHYS_ADDR(cursor->module->begin_address +
                                                                *function_name_rva);

                                                    if ( VALID_PTR(function_name) )
                                                    {
                                                        argos_logf("Called %s from %s\n", function_name,
                                                                cursor->module->name);
                                                    }
                                                    else
                                                    {
                                                        argos_logf("Invalid pointer to function name!!!\n");
                                                    }
                                                }
                                                else
                                                {
                                                    argos_logf("Called @%u from %s\n", *function_ordinal,
                                                            cursor->module->name);
                                                }
                                                break;
                                            }
                                        }
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                        cursor = cursor->next;
                    }

                    if ( cursor == NULL )
                    {
                        argos_logf(
                                "Called non-imported function at 0x%08x\n",
                                env->eip);
                    }
                }
            }
        }
        else
        {
            argos_logf("Called injected function at 0x%08x.\n",
                    env->eip);
        }
    }
exit_call_check:
    if (sigprocmask(SIG_UNBLOCK, &alrmset, NULL) != 0)
        perror("could not unblock temporarily blocked signals");
}

static target_ulong _get_current_thread_id(CPUX86State * env)
{
    // fs:[0x18] contains a flat pointer to the Thread Execution Block.
    target_phys_addr_t ptr_teb =
        PHYS_ADDR(env->segs[R_FS].base);
    if ( !VALID_PTR(ptr_teb))
    {
        argos_logf("Failed to obtain physical address of teb.\n");
        return 0;
    }

    return *((target_ulong*)(ptr_teb + 0x24));
}

static unsigned char _in_shellcode_context(CPUX86State * env)
{
    if ( env->cr[3] == env->shellcode_context.cr3 )
    {
        target_ulong thread_id = _get_current_thread_id(env);
        if ( thread_id == env->shellcode_context.thread_id )
        {
            return 1;
        }
    }
    return 0;
}

static int _get_instr_len(unsigned long pc)
{
    return get_instruction(&current_instr, (BYTE *)pc, MODE_32);
}

static void _log_instr(argos_shellcode_context_t * context)
{
    char instr_str[1024];
    if (get_instruction_string(&current_instr, FORMAT_INTEL, 0, instr_str,
                sizeof(instr_str)))
    {
        fprintf(context->logfile, "%s", instr_str);
    }
}

static argos_tracksc_exported_functions * _get_exports_from_module(CPUX86State * env,
        argos_tracksc_imported_module * module)
{
    argos_tracksc_exported_functions * exports =
        (argos_tracksc_exported_functions *)
        malloc(sizeof(argos_tracksc_exported_functions));

    if ( exports == NULL )
        return NULL;

    memset(exports, 0, sizeof(argos_tracksc_exported_functions));

    target_ulong function_address_table = module->function_address_table_rva
        + module->begin_address;

    target_ulong function_ordinal_table = module->function_ordinal_table_rva
        + module->begin_address;

    target_ulong function_name_table = module->function_name_table_rva
        + module->begin_address;

    unsigned i = 0;
    for(; i < module->number_of_functions; ++i)
    {
        argos_tracksc_exported_function * export =
            (argos_tracksc_exported_function *)
            malloc(sizeof(argos_tracksc_exported_function));

        if ( export == NULL )
            continue;

        memset(export, 0, sizeof(argos_tracksc_exported_function));

        if ( i < module->number_of_functions_with_names )
        {
            target_ulong * function_name_rva = (target_ulong*)
                PHYS_ADDR(function_name_table + i * sizeof(target_ulong));

            if ( VALID_PTR(function_name_rva) )
            {
                char * function_name = (char *)PHYS_ADDR(*function_name_rva
                        + module->begin_address);
                if ( VALID_PTR(function_name) )
                {
                    export->name = strdup(function_name);
                    argos_logf("%s\n", function_name);
                }
            }
        }
        else
        {
        }
    }
}

static argos_tracksc_imported_module * _get_module(CPUX86State * env, target_ulong base_address)
{
    // IMAGE_DOS_HEADER
    target_phys_addr_t dos_hdr = PHYS_ADDR(base_address);

    if ( !VALID_PTR(dos_hdr) )
    {
        argos_logf("Invalid base address ptr\n");
        return NULL;
    }

    // IMAGE_DOS_HEADER.e_magic
    if ( *(uint16_t*)dos_hdr != IMAGE_DOS_SIGNATURE  )
    {
        argos_logf("Module has an invalid dos signature.\n");
        return NULL;
    }

    // IMAGE_NT_HEADERS
    target_phys_addr_t pe_hdr =
        PHYS_ADDR(base_address
                + *(target_ulong*)(dos_hdr+0x3c));

    if ( !VALID_PTR(pe_hdr) )
    {
        argos_logf("Invalid base address ptr\n");
        return NULL;
    }

    // IMAGE_NT_HEADERS.Signature
    if ( *(target_ulong*)pe_hdr != IMAGE_PE_SIGNATURE )
    {
        argos_logf("Module has an invalid pe signature.\n");
        return NULL;
    }

    // IMAGE_OPTIONAL_HEADER.SizeOfImage;
    target_ulong size_of_image =
        *(target_ulong*)(pe_hdr + 0x50);

    target_ulong end_address = base_address + size_of_image;

    // IMAGE_OPTIONAL_HEADER.NumberOfRvaAndSizes
    if ( *(target_ulong*)(pe_hdr + 0x74) == 0 )
    {
        argos_logf("Module has no sections!\n");
        return NULL;
    }

    argos_tracksc_imported_module * imported_module =
        (argos_tracksc_imported_module *)
        malloc(sizeof(argos_tracksc_imported_module));

    if ( imported_module == NULL)
    {
        argos_logf("Unable to allocate memory for analysis.\n");
        return NULL;
    }

    imported_module->begin_address = base_address;
    imported_module->end_address = end_address;

    // IMAGE_OPTIONAL_HEADER.DataDirectory[0].VirtualAddress
    target_ulong export_directory_virtual_address =
        *(target_ulong*)(pe_hdr + 0x78);

    if ( export_directory_virtual_address != 0 )
    {
        // IMAGE_EXPORT_DIR.Name
        target_ulong name_address =
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0xC);

        char * name = (char *)PHYS_ADDR(base_address + name_address);
        if ( VALID_PTR(name) )
        {
            strncpy(imported_module->name, name, 255);
            imported_module->name[255] = '\0';
        }
        else
        {
            imported_module->name[0] = '\0';
        }

        // IMAGE_EXPORT_DIR.Base
        target_ulong base =
            //*(target_ulong*)(export_dir + 0x10);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x10);

        // IMAGE_EXPORT_DIR.NumberOfFunctions
        target_ulong number_of_functions =
            //*(target_ulong*)(export_dir + 0x14);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x14);

        // IMAGE_EXPORT_DIR.NumberOfNames
        target_ulong number_of_functions_with_names =
            //*(target_ulong*)(export_dir + 0x18);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x18);

        // IMAGE_EXPORT_DIR.AddressOfFunctions
        target_ulong function_address_table_rva =
            //*(target_ulong*)(export_dir + 0x1C);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x1C);

        // IMAGE_EXPORT_DIR.AddressOfNames
        target_ulong function_name_table_rva =
            //*(target_ulong*)(export_dir + 0x1C);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x20);

        // IMAGE_EXPORT_DIR.AddressOfNameOrdinals
        target_ulong function_ordinal_table_rva =
            //*(target_ulong*)(export_dir + 0x24);
            *(target_ulong*)PHYS_ADDR(base_address
                    + export_directory_virtual_address + 0x24);

        imported_module->number_of_functions = number_of_functions;
        imported_module->number_of_functions_with_names = number_of_functions_with_names;
        imported_module->function_address_table_rva = function_address_table_rva;
        imported_module->function_name_table_rva = function_name_table_rva;
        imported_module->function_ordinal_table_rva = function_ordinal_table_rva;
        imported_module->base_ordinal = base;

        argos_logf("%s: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x %04u %04u\n",
                imported_module->name,
                base_address,
                function_address_table_rva, function_ordinal_table_rva, function_name_table_rva,
                end_address,
                number_of_functions,
                number_of_functions_with_names);

        //_get_exports_from_module(env, imported_module);
    }
    else
    {
        imported_module->name[0] = '\0';
        imported_module->number_of_functions = 0;
        imported_module->number_of_functions_with_names = 0;
        imported_module->function_address_table_rva = 0;
        imported_module->function_name_table_rva = 0;
        imported_module->function_ordinal_table_rva = 0;
        imported_module->base_ordinal = 0;
    }

    return imported_module;
}

// Description: Converts UTF-16 to UTF-8, the quick and dirty way.
// Returns the number of converted characters.
// Note: The source_length is the length EXCLUDING the zero terminating
// characters, but the destination size is the size of the buffer INCLUDING
// the zero terminating character. The source and destination pointers MUST
// BE valid pointers.
static uint32_t _utf16_to_utf8(uint16_t * source, uint32_t source_length,
        char * destination, uint32_t destination_size)
{
   uint32_t i = 0, number_converted_characters = 0;
   for (; i < source_length && i < destination_size; ++i)
   {
       uint16_t uc = source[i];
       if ( (uc & 0xFF00) == 0 )
       {
           destination[i] = (uc & 0xFF);
           number_converted_characters++;
       }
       else
       {
           destination[i] = '?';
       }
   }
   destination[destination_size-1] = '\0';
   return number_converted_characters;
}

static void _get_imported_modules(CPUX86State * env)
{
    target_ulong vaddr_of_teb = env->segs[R_FS].base;

    // _TEB;
    target_phys_addr_t paddr_of_teb = PHYS_ADDR(vaddr_of_teb);
    if (!VALID_PTR(paddr_of_teb))
    {
        argos_logf("Invalid pointer to teb: 0x%016lx\n", paddr_of_teb);
    }

    // _TEB.ProcessEnvironmentBlock
    target_ulong  vaddr_of_peb = *((target_ulong*) (paddr_of_teb + 0x30));

    // _PEB
    target_phys_addr_t paddr_of_peb = PHYS_ADDR(vaddr_of_peb);
    if (!VALID_PTR(paddr_of_peb))
    {
        argos_logf("Invalid pointer to peb: 0x%016lx\n", paddr_of_teb);
    }

    // _PEB.ImageBaseAddress
    //target_ulong target_base = *((target_ulong*)(paddr_of_peb + 0x8));

    // _PEB.Ldr
    target_ulong vaddr_ldr_data = *((target_ulong*)(paddr_of_peb + 0xc));

    // _PEB_LDR_DATA
    target_phys_addr_t paddr_ldr_data =
        PHYS_ADDR(vaddr_ldr_data);
    if (!VALID_PTR(paddr_ldr_data))
    {
        argos_logf("Invalid pointer to LdrData: 0x%016lx\n",
                paddr_ldr_data);
    }

    uint8_t initialized = *((uint8_t*)paddr_ldr_data + 0x4);

    if ( initialized )
    {
        target_phys_addr_t module_list = paddr_ldr_data + 0xc;

        target_phys_addr_t ldr_data_entry =
            PHYS_ADDR(*(target_ulong*)module_list);

        argos_tracksc_imported_modules * last_imported_entry = NULL;
        //argos_logf("%10s\t%10s\t%50s\t%5s\n", "begin:", "end:", "module:", "exports:");

        while(1)
        {
            char module_basename[255];

            if ( VALID_PTR(ldr_data_entry) )
            {
                target_ulong module_base =
                    *(target_ulong*)(ldr_data_entry + 0x18);

                // _LDR_DATA_TABLE_ENTRY.BaseDllName.Length
                uint16_t basename_length =
                    (*(uint16_t*)(ldr_data_entry + 0x2c))
                    / sizeof(uint16_t);
                if ( basename_length > 0 && basename_length < 256 )
                {
                    uint16_t * basename_buffer =
                        (uint16_t *)PHYS_ADDR(
                                (*(target_ulong*)(ldr_data_entry + 0x30)));

                    if ( VALID_PTR(basename_buffer) )
                    {
                        _utf16_to_utf8(basename_buffer, basename_length,
                                module_basename, sizeof(module_basename));
                    }
                }

                argos_tracksc_imported_modules * imported_module_entry =
                    (argos_tracksc_imported_modules *)
                    malloc(sizeof(argos_tracksc_imported_modules));

                if ( imported_module_entry == NULL)
                {
                    argos_logf("Unable to allocate memory for analysis!!");
                    exit(1);
                }

                imported_module_entry->next = NULL;

                argos_tracksc_imported_module * imported_module =
                    _get_module(env, module_base);

                if ( imported_module == NULL )
                {
                    free(imported_module_entry);
                    goto next_module;
                }

                imported_module_entry->module = imported_module;
                if ( last_imported_entry )
                {
                    last_imported_entry =
                        last_imported_entry->next = imported_module_entry;
                }
                else
                {
                    env->shellcode_context.imported_modules =
                        last_imported_entry = imported_module_entry;
                }

                /*argos_logf("0x%010u\t0x%010u\t%30s\t%5u\n", module_base,
                        imported_module->end_address, module_basename,
                        imported_module->number_of_functions);*/

next_module:
                ldr_data_entry =
                    PHYS_ADDR(*(target_ulong*)ldr_data_entry);

                if ( ldr_data_entry == module_list )
                {
                    break;
                }
            }
        }
    }

    return;

    /*target_phys_addr_t offset_of_peb =

    LIST_ENTRY loaded_modules = ppeb_ldr_data->InLoadOrderModuleList;

    argos_logf("LIST_ENTRY->Flink: 0x%x\n", loaded_modules.Flink);
    LIST_ENTRY * fwd_iter = (LIST_ENTRY *)PHYS_OFFSET(loaded_modules.Flink);
    if ( !VALID_PHYS_OFFSET(fwd_iter) )
    {
        argos_logf("Failed to obtain physical address of Flink.\n");
        return;
    }

    LDR_DATA_TABLE_ENTRY * module_table = (LDR_DATA_TABLE_ENTRY *)fwd_iter;
    argos_tracksc_imported_function * last_import = NULL;
    while ( module_table->BaseAddress != 0 )
    {
        argos_logf("Module base: 0x%x\n", module_table->BaseAddress);

        IMAGE_DOS_HEADER * dos_hdr = (IMAGE_DOS_HEADER *)
            PHYS_OFFSET(module_table->BaseAddress);
        if ( !VALID_PHYS_OFFSET(dos_hdr) )
        {
            argos_logf("Failed to obtain physical address \
                    of dos header.\n");
            goto next;
        }

        if ( dos_hdr->e_magic != IMAGE_DOS_SIGNATURE  )
        {
            argos_logf("Imported module has an invalid dos signature.\n");
            goto next;
        }

        IMAGE_NT_HEADERS * pe_hdr = (IMAGE_NT_HEADERS *)
            PHYS_OFFSET(module_table->BaseAddress
                    + dos_hdr->e_lfanew);
        if ( !VALID_PHYS_OFFSET(pe_hdr) )
        {
            argos_logf("Failed to obtain physical address of pe header.\n");
            goto next;
        }

        if ( pe_hdr->Signature != IMAGE_PE_SIGNATURE )
        {
            argos_logf("Imported module has an invalid pe signature.\n");
            goto next;
        }


        if ( pe_hdr->OptionalHeader.NumberOfRvaAndSizes == 0 )
        {
            goto next;
        }

        IMAGE_DATA_DIRECTORY export_data_dir
            = pe_hdr->OptionalHeader.DataDirectory[0];
        if ( export_data_dir.VirtualAddress == 0 )
        {
            argos_logf("Module has no export table!\n");
            goto next;
        }

        IMAGE_EXPORT_DIRECTORY * export_dir =
            (IMAGE_EXPORT_DIRECTORY *)
            PHYS_OFFSET(module_table->BaseAddress +
                    export_data_dir.VirtualAddress);
        if ( !VALID_PHYS_OFFSET(export_dir) )
        {
            argos_logf("Failed to obtain physical address of export \
                    directory.\n");
            goto next;
        }

        target_ulong addresses =
            module_table->BaseAddress
            + export_dir->AddressOfFunctions;

        target_ulong names =
            module_table->BaseAddress
            + export_dir->AddressOfNames;

        target_ulong nameOrdinals =
            module_table->BaseAddress
            + export_dir->AddressOfNameOrdinals;


        char * module_name =
            (char*)PHYS_OFFSET(module_table->BaseAddress
                    + export_dir->Name);

        if ( !VALID_PHYS_OFFSET(module_name) )
        {
            argos_logf("Failed to obtain physical address \
                    of module name.\n");
            goto next;
        }

        printf("Found module %s\n", module_name);
        unsigned i;
        for (i = 0; i < export_dir->NumberOfFunctions; i++)
        {
            target_phys_addr_t function_ordinal_addr = 0;
            argos_tracksc_imported_function
                * import =
                malloc( sizeof
                        (
                         argos_tracksc_imported_function
                        ));

            if ( import == NULL )
            {
                argos_logf("Failed to allocate memory to \
                        store imported functions!!!\n");
                return;
            }
            memset(import, 0, sizeof(argos_tracksc_imported_function));

            import->module = strdup(module_name);

            if ( i < export_dir->NumberOfNames)
            {
                target_phys_addr_t function_name_rva =
                    PHYS_OFFSET(names + i * sizeof(DWORD) );
                if ( VALID_PHYS_OFFSET(function_name_rva) )
                {
                    target_phys_addr_t function_name_addr =
                        PHYS_OFFSET( module_table->BaseAddress
                                + *((DWORD*)function_name_rva));

                    if (VALID_PHYS_OFFSET(function_name_addr))
                    {
                        char * function_name =
                            (char*)function_name_addr;
                        import->function =
                            strdup(function_name);
                    }
                    else
                    {
                        argos_logf("Invalid physical address of function \
                                name.\n");
                    }
                }
                else
                {
                    argos_logf("Invalid physical address of function name \
                            rva.\n");
                }
            }

            function_ordinal_addr = PHYS_OFFSET(nameOrdinals
                    + i * sizeof(WORD) );
            if ( VALID_PHYS_OFFSET(function_ordinal_addr) )
            {
                target_phys_addr_t function_paddr = 0;
                WORD ordinal = *((WORD*)function_ordinal_addr);
                import->ordinal = ordinal + export_dir->Base;

                function_paddr = PHYS_OFFSET(addresses +
                        (ordinal - export_dir->Base) * sizeof(DWORD));
                if ( VALID_PHYS_OFFSET(function_paddr) )
                {
                    target_ulong function_addr = module_table->BaseAddress
                        + *((DWORD*)function_paddr);
                    import->address = function_addr;
                }
                else
                {
                    argos_logf("Invalid physical address of function \
                            address.\n");
                }
            }
            else
            {
                argos_logf("Invalid physical address of function \
                        ordinal.\n");
            }

            if ( last_import )
            {
                last_import = last_import->next = import;
            }
            else
            {
                // If last_import is not set we assume that the no 
                // imported functions are added to the list.
                env->shellcode_context.imported_functions =
                    last_import = import;
            }
        }

next:
        fwd_iter = (LIST_ENTRY *) PHYS_OFFSET(fwd_iter->Flink);
        if ( !VALID_PHYS_OFFSET(fwd_iter) )
        {
            argos_logf("Failed to obtain physical address of next module.\n");
            return;
        }
        module_table = (LDR_DATA_TABLE_ENTRY *)fwd_iter;
    }*/
}

static void _save_shellcode_context(CPUX86State * env)
{
    env->shellcode_context.cr3 = env->cr[3];
    env->shellcode_context.thread_id = _get_current_thread_id(env);
    argos_logf("Context: CR3 = 0x%x Thead Id = %u.\n",
            env->shellcode_context.cr3,
            env->shellcode_context.thread_id);
}

static void _start_analysis_phase(CPUX86State * env)
{
    env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_ANALYZING;

   argos_logf("Dumping loaded modules for targeted process.\n");
   _get_imported_modules(env);
   argos_logf("Done dumping loaded modules for targeted process.\n");
}

static void _start_tracking_phase(CPUX86State * env)
{
    env->shellcode_context.phase = ARGOS_TRACKSC_PHASE_TRACKING;
    // We have successfully initialized the shellcode context,
    // now put the cpu in single step mode to log the shellcode
    // instructions.
    argos_logf("Successfully instanciated shellcode tracking, "
            "switching to single step mode.\n");
    // Enabling single stepping mode allows us to log
    // the shellcode instructions in the cpu loop.
    // With single step mode enabled the TB's contain
    // only one instruction and an exception is thrown
    // after the execution of this instruction.
    // During the handling of this exception (EXCP_DEBUG)
    // we can log the instruction in the current TB.
    cpu_single_step(env, 1);
}

static unsigned char _is_tracking(CPUX86State * env)
{
    return env->shellcode_context.phase == ARGOS_TRACKSC_PHASE_TRACKING;
}
