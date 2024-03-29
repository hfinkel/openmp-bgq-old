/*
 * kmp_affinity.cpp -- affinity management
 */


//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//


#include "kmp.h"
#include "kmp_i18n.h"
#include "kmp_io.h"
#include "kmp_str.h"
#include "kmp_wrapper_getpid.h"
#include "kmp_affinity.h"

// Store the real or imagined machine hierarchy here
static hierarchy_info machine_hierarchy;

void __kmp_cleanup_hierarchy() {
    machine_hierarchy.fini();
}

void __kmp_get_hierarchy(kmp_uint32 nproc, kmp_bstate_t *thr_bar) {
    kmp_uint32 depth;
    // The test below is true if affinity is available, but set to "none". Need to init on first use of hierarchical barrier.
    if (TCR_1(machine_hierarchy.uninitialized))
        machine_hierarchy.init(NULL, nproc);

    depth = machine_hierarchy.depth;
    KMP_DEBUG_ASSERT(depth > 0);
    // Adjust the hierarchy in case num threads exceeds original
    if (nproc > machine_hierarchy.skipPerLevel[depth-1])
        machine_hierarchy.resize(nproc);

    thr_bar->depth = depth;
    thr_bar->base_leaf_kids = (kmp_uint8)machine_hierarchy.numPerLevel[0]-1;
    thr_bar->skip_per_level = machine_hierarchy.skipPerLevel;
}

#if KMP_AFFINITY_SUPPORTED

//
// Print the affinity mask to the character array in a pretty format.
//
char *
__kmp_affinity_print_mask(char *buf, int buf_len, kmp_affin_mask_t *mask)
{
    KMP_ASSERT(buf_len >= 40);
    char *scan = buf;
    char *end = buf + buf_len - 1;

    //
    // Find first element / check for empty set.
    //
    size_t i;
    for (i = 0; i < KMP_CPU_SETSIZE; i++) {
        if (KMP_CPU_ISSET(i, mask)) {
            break;
        }
    }
    if (i == KMP_CPU_SETSIZE) {
        KMP_SNPRINTF(scan, buf_len, "{<empty>}");
        while (*scan != '\0') scan++;
        KMP_ASSERT(scan <= end);
        return buf;
    }

    KMP_SNPRINTF(scan, buf_len, "{%ld", (long)i);
    while (*scan != '\0') scan++;
    i++;
    for (; i < KMP_CPU_SETSIZE; i++) {
        if (! KMP_CPU_ISSET(i, mask)) {
            continue;
        }

        //
        // Check for buffer overflow.  A string of the form ",<n>" will have
        // at most 10 characters, plus we want to leave room to print ",...}"
        // if the set is too large to print for a total of 15 characters.
        // We already left room for '\0' in setting end.
        //
        if (end - scan < 15) {
           break;
        }
        KMP_SNPRINTF(scan, buf_len, ",%-ld", (long)i);
        while (*scan != '\0') scan++;
    }
    if (i < KMP_CPU_SETSIZE) {
        KMP_SNPRINTF(scan, buf_len,  ",...");
        while (*scan != '\0') scan++;
    }
    KMP_SNPRINTF(scan, buf_len, "}");
    while (*scan != '\0') scan++;
    KMP_ASSERT(scan <= end);
    return buf;
}


void
__kmp_affinity_entire_machine_mask(kmp_affin_mask_t *mask)
{
    KMP_CPU_ZERO(mask);

# if KMP_GROUP_AFFINITY

    if (__kmp_num_proc_groups > 1) {
        int group;
        KMP_DEBUG_ASSERT(__kmp_GetActiveProcessorCount != NULL);
        for (group = 0; group < __kmp_num_proc_groups; group++) {
            int i;
            int num = __kmp_GetActiveProcessorCount(group);
            for (i = 0; i < num; i++) {
                KMP_CPU_SET(i + group * (CHAR_BIT * sizeof(DWORD_PTR)), mask);
            }
        }
    }
    else

# endif /* KMP_GROUP_AFFINITY */

    {
        int proc;
        for (proc = 0; proc < __kmp_xproc; proc++) {
            KMP_CPU_SET(proc, mask);
        }
    }
}

//
// When sorting by labels, __kmp_affinity_assign_child_nums() must first be
// called to renumber the labels from [0..n] and place them into the child_num
// vector of the address object.  This is done in case the labels used for
// the children at one node of the hierarchy differ from those used for
// another node at the same level.  Example:  suppose the machine has 2 nodes
// with 2 packages each.  The first node contains packages 601 and 602, and
// second node contains packages 603 and 604.  If we try to sort the table
// for "scatter" affinity, the table will still be sorted 601, 602, 603, 604
// because we are paying attention to the labels themselves, not the ordinal
// child numbers.  By using the child numbers in the sort, the result is
// {0,0}=601, {0,1}=603, {1,0}=602, {1,1}=604.
//
static void
__kmp_affinity_assign_child_nums(AddrUnsPair *address2os,
  int numAddrs)
{
    KMP_DEBUG_ASSERT(numAddrs > 0);
    int depth = address2os->first.depth;
    unsigned *counts = (unsigned *)__kmp_allocate(depth * sizeof(unsigned));
    unsigned *lastLabel = (unsigned *)__kmp_allocate(depth
      * sizeof(unsigned));
    int labCt;
    for (labCt = 0; labCt < depth; labCt++) {
        address2os[0].first.childNums[labCt] = counts[labCt] = 0;
        lastLabel[labCt] = address2os[0].first.labels[labCt];
    }
    int i;
    for (i = 1; i < numAddrs; i++) {
        for (labCt = 0; labCt < depth; labCt++) {
            if (address2os[i].first.labels[labCt] != lastLabel[labCt]) {
                int labCt2;
                for (labCt2 = labCt + 1; labCt2 < depth; labCt2++) {
                    counts[labCt2] = 0;
                    lastLabel[labCt2] = address2os[i].first.labels[labCt2];
                }
                counts[labCt]++;
                lastLabel[labCt] = address2os[i].first.labels[labCt];
                break;
            }
        }
        for (labCt = 0; labCt < depth; labCt++) {
            address2os[i].first.childNums[labCt] = counts[labCt];
        }
        for (; labCt < (int)Address::maxDepth; labCt++) {
            address2os[i].first.childNums[labCt] = 0;
        }
    }
}


//
// All of the __kmp_affinity_create_*_map() routines should set
// __kmp_affinity_masks to a vector of affinity mask objects of length
// __kmp_affinity_num_masks, if __kmp_affinity_type != affinity_none, and
// return the number of levels in the machine topology tree (zero if
// __kmp_affinity_type == affinity_none).
//
// All of the __kmp_affinity_create_*_map() routines should set *fullMask
// to the affinity mask for the initialization thread.  They need to save and
// restore the mask, and it could be needed later, so saving it is just an
// optimization to avoid calling kmp_get_system_affinity() again.
//
static kmp_affin_mask_t *fullMask = NULL;

kmp_affin_mask_t *
__kmp_affinity_get_fullMask() { return fullMask; }


static int nCoresPerPkg, nPackages;
static int __kmp_nThreadsPerCore;
#ifndef KMP_DFLT_NTH_CORES
static int __kmp_ncores;
#endif

//
// __kmp_affinity_uniform_topology() doesn't work when called from
// places which support arbitrarily many levels in the machine topology
// map, i.e. the non-default cases in __kmp_affinity_create_cpuinfo_map()
// __kmp_affinity_create_x2apicid_map().
//
inline static bool
__kmp_affinity_uniform_topology()
{
    return __kmp_avail_proc == (__kmp_nThreadsPerCore * nCoresPerPkg * nPackages);
}


//
// Print out the detailed machine topology map, i.e. the physical locations
// of each OS proc.
//
static void
__kmp_affinity_print_topology(AddrUnsPair *address2os, int len, int depth,
  int pkgLevel, int coreLevel, int threadLevel)
{
    int proc;

    KMP_INFORM(OSProcToPhysicalThreadMap, "KMP_AFFINITY");
    for (proc = 0; proc < len; proc++) {
        int level;
        kmp_str_buf_t buf;
        __kmp_str_buf_init(&buf);
        for (level = 0; level < depth; level++) {
            if (level == threadLevel) {
                __kmp_str_buf_print(&buf, "%s ", KMP_I18N_STR(Thread));
            }
            else if (level == coreLevel) {
                __kmp_str_buf_print(&buf, "%s ", KMP_I18N_STR(Core));
            }
            else if (level == pkgLevel) {
                __kmp_str_buf_print(&buf, "%s ", KMP_I18N_STR(Package));
            }
            else if (level > pkgLevel) {
                __kmp_str_buf_print(&buf, "%s_%d ", KMP_I18N_STR(Node),
                  level - pkgLevel - 1);
            }
            else {
                __kmp_str_buf_print(&buf, "L%d ", level);
            }
            __kmp_str_buf_print(&buf, "%d ",
              address2os[proc].first.labels[level]);
        }
        KMP_INFORM(OSProcMapToPack, "KMP_AFFINITY", address2os[proc].second,
          buf.str);
        __kmp_str_buf_free(&buf);
    }
}

#if KMP_OS_CNK
// Set the default topology information for an IBM BG/Q
static int
__kmp_affinity_create_bgq_map(AddrUnsPair **address2os,
  kmp_i18n_id_t *const msg_id)
{
    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    // Fall back to the flat map if we're not using affinity now.
    if (! KMP_AFFINITY_CAPABLE() /*|| __kmp_affinity_type == affinity_none*/) {
      return -1;
    }

    // Each BG/Q node has only one package, which has 16 cores, and 4 hardware
    // threads per core.
    nPackages = 1;
    __kmp_ncores = nCoresPerPkg = 16;
    __kmp_nThreadsPerCore = 4;

    if (__kmp_affinity_verbose) {
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, fullMask);

        if (__kmp_affinity_respect_mask) {
            KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
        } else {
            KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
        }
        KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
        if (__kmp_affinity_uniform_topology()) {
            KMP_INFORM(Uniform, "KMP_AFFINITY");
        } else {
            KMP_INFORM(NonUniform, "KMP_AFFINITY");
        }
        KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
          __kmp_nThreadsPerCore, __kmp_ncores);
    }

    //
    // Contruct the data structure to be returned.
    //
    int depth = 3;
    *address2os = (AddrUnsPair*)
      __kmp_allocate(sizeof(**address2os) * __kmp_avail_proc);
    int avail_ct = 0;
    unsigned int i;
    for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
        //
        // Skip this proc if it is not included in the machine model.
        //
        if (! KMP_CPU_ISSET(i, fullMask)) {
            continue;
        }

        Address addr(depth);
        // Note: Levels are inverted here.
        addr.labels[2] = 0;
        addr.labels[1] = i%4;
        addr.labels[0] = i/4;
        (*address2os)[avail_ct++] = AddrUnsPair(addr,i);
    }

    if (__kmp_affinity_verbose) {
        __kmp_affinity_print_topology(*address2os, __kmp_avail_proc, depth, 2, 1, 0);
    }

    if (__kmp_affinity_gran_levels < 0) {
        if (__kmp_affinity_gran > affinity_gran_thread) {
            __kmp_affinity_gran_levels = 3;
        }
        else if (__kmp_affinity_gran > affinity_gran_core) {
            __kmp_affinity_gran_levels = 2;
        }
        else if (__kmp_affinity_gran > affinity_gran_package) {
            __kmp_affinity_gran_levels = 1;
        }
        else {
            __kmp_affinity_gran_levels = 0;
        }
    }
    return depth;
}
#endif // KMP_OS_CNK

//
// If we don't know how to retrieve the machine's processor topology, or
// encounter an error in doing so, this routine is called to form a "flat"
// mapping of os thread id's <-> processor id's.
//
static int
__kmp_affinity_create_flat_map(AddrUnsPair **address2os,
  kmp_i18n_id_t *const msg_id)
{
    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    //
    // Even if __kmp_affinity_type == affinity_none, this routine might still
    // called to set __kmp_ncores, as well as
    // __kmp_nThreadsPerCore, nCoresPerPkg, & nPackages.
    //
    if (! KMP_AFFINITY_CAPABLE()) {
        KMP_ASSERT(__kmp_affinity_type == affinity_none);
        __kmp_ncores = nPackages = __kmp_xproc;
        __kmp_nThreadsPerCore = nCoresPerPkg = 1;
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffFlatTopology, "KMP_AFFINITY");
            KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
            KMP_INFORM(Uniform, "KMP_AFFINITY");
            KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
              __kmp_nThreadsPerCore, __kmp_ncores);
        }
        return 0;
    }

    //
    // When affinity is off, this routine will still be called to set
    // __kmp_ncores, as well as __kmp_nThreadsPerCore,
    // nCoresPerPkg, & nPackages.  Make sure all these vars are set
    //  correctly, and return now if affinity is not enabled.
    //
    __kmp_ncores = nPackages = __kmp_avail_proc;
    __kmp_nThreadsPerCore = nCoresPerPkg = 1;
    if (__kmp_affinity_verbose) {
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, fullMask);

        KMP_INFORM(AffCapableUseFlat, "KMP_AFFINITY");
        if (__kmp_affinity_respect_mask) {
            KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
        } else {
            KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
        }
        KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
        KMP_INFORM(Uniform, "KMP_AFFINITY");
        KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
          __kmp_nThreadsPerCore, __kmp_ncores);
    }
    if (__kmp_affinity_type == affinity_none) {
        return 0;
    }

    //
    // Contruct the data structure to be returned.
    //
    *address2os = (AddrUnsPair*)
      __kmp_allocate(sizeof(**address2os) * __kmp_avail_proc);
    int avail_ct = 0;
    unsigned int i;
    for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
        //
        // Skip this proc if it is not included in the machine model.
        //
        if (! KMP_CPU_ISSET(i, fullMask)) {
            continue;
        }

        Address addr(1);
        addr.labels[0] = i;
        (*address2os)[avail_ct++] = AddrUnsPair(addr,i);
    }
    if (__kmp_affinity_verbose) {
        KMP_INFORM(OSProcToPackage, "KMP_AFFINITY");
    }

    if (__kmp_affinity_gran_levels < 0) {
        //
        // Only the package level is modeled in the machine topology map,
        // so the #levels of granularity is either 0 or 1.
        //
        if (__kmp_affinity_gran > affinity_gran_package) {
            __kmp_affinity_gran_levels = 1;
        }
        else {
            __kmp_affinity_gran_levels = 0;
        }
    }
    return 1;
}


# if KMP_GROUP_AFFINITY

//
// If multiple Windows* OS processor groups exist, we can create a 2-level
// topology map with the groups at level 0 and the individual procs at
// level 1.
//
// This facilitates letting the threads float among all procs in a group,
// if granularity=group (the default when there are multiple groups).
//
static int
__kmp_affinity_create_proc_group_map(AddrUnsPair **address2os,
  kmp_i18n_id_t *const msg_id)
{
    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    //
    // If we don't have multiple processor groups, return now.
    // The flat mapping will be used.
    //
    if ((! KMP_AFFINITY_CAPABLE()) || (__kmp_get_proc_group(fullMask) >= 0)) {
        // FIXME set *msg_id
        return -1;
    }

    //
    // Contruct the data structure to be returned.
    //
    *address2os = (AddrUnsPair*)
      __kmp_allocate(sizeof(**address2os) * __kmp_avail_proc);
    int avail_ct = 0;
    int i;
    for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
        //
        // Skip this proc if it is not included in the machine model.
        //
        if (! KMP_CPU_ISSET(i, fullMask)) {
            continue;
        }

        Address addr(2);
        addr.labels[0] = i / (CHAR_BIT * sizeof(DWORD_PTR));
        addr.labels[1] = i % (CHAR_BIT * sizeof(DWORD_PTR));
        (*address2os)[avail_ct++] = AddrUnsPair(addr,i);

        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffOSProcToGroup, "KMP_AFFINITY", i, addr.labels[0],
              addr.labels[1]);
        }
    }

    if (__kmp_affinity_gran_levels < 0) {
        if (__kmp_affinity_gran == affinity_gran_group) {
            __kmp_affinity_gran_levels = 1;
        }
        else if ((__kmp_affinity_gran == affinity_gran_fine)
          || (__kmp_affinity_gran == affinity_gran_thread)) {
            __kmp_affinity_gran_levels = 0;
        }
        else {
            const char *gran_str = NULL;
            if (__kmp_affinity_gran == affinity_gran_core) {
                gran_str = "core";
            }
            else if (__kmp_affinity_gran == affinity_gran_package) {
                gran_str = "package";
            }
            else if (__kmp_affinity_gran == affinity_gran_node) {
                gran_str = "node";
            }
            else {
                KMP_ASSERT(0);
            }

            // Warning: can't use affinity granularity \"gran\" with group topology method, using "thread"
            __kmp_affinity_gran_levels = 0;
        }
    }
    return 2;
}

# endif /* KMP_GROUP_AFFINITY */


# if KMP_ARCH_X86 || KMP_ARCH_X86_64

static int
__kmp_cpuid_mask_width(int count) {
    int r = 0;

    while((1<<r) < count)
        ++r;
    return r;
}


class apicThreadInfo {
public:
    unsigned osId;              // param to __kmp_affinity_bind_thread
    unsigned apicId;            // from cpuid after binding
    unsigned maxCoresPerPkg;    //      ""
    unsigned maxThreadsPerPkg;  //      ""
    unsigned pkgId;             // inferred from above values
    unsigned coreId;            //      ""
    unsigned threadId;          //      ""
};


static int
__kmp_affinity_cmp_apicThreadInfo_os_id(const void *a, const void *b)
{
    const apicThreadInfo *aa = (const apicThreadInfo *)a;
    const apicThreadInfo *bb = (const apicThreadInfo *)b;
    if (aa->osId < bb->osId) return -1;
    if (aa->osId > bb->osId) return 1;
    return 0;
}


static int
__kmp_affinity_cmp_apicThreadInfo_phys_id(const void *a, const void *b)
{
    const apicThreadInfo *aa = (const apicThreadInfo *)a;
    const apicThreadInfo *bb = (const apicThreadInfo *)b;
    if (aa->pkgId < bb->pkgId) return -1;
    if (aa->pkgId > bb->pkgId) return 1;
    if (aa->coreId < bb->coreId) return -1;
    if (aa->coreId > bb->coreId) return 1;
    if (aa->threadId < bb->threadId) return -1;
    if (aa->threadId > bb->threadId) return 1;
    return 0;
}


//
// On IA-32 architecture and Intel(R) 64 architecture, we attempt to use
// an algorithm which cycles through the available os threads, setting
// the current thread's affinity mask to that thread, and then retrieves
// the Apic Id for each thread context using the cpuid instruction.
//
static int
__kmp_affinity_create_apicid_map(AddrUnsPair **address2os,
  kmp_i18n_id_t *const msg_id)
{
    kmp_cpuid buf;
    int rc;
    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    //
    // Check if cpuid leaf 4 is supported.
    //
        __kmp_x86_cpuid(0, 0, &buf);
        if (buf.eax < 4) {
            *msg_id = kmp_i18n_str_NoLeaf4Support;
            return -1;
        }

    //
    // The algorithm used starts by setting the affinity to each available
    // thread and retrieving info from the cpuid instruction, so if we are
    // not capable of calling __kmp_get_system_affinity() and
    // _kmp_get_system_affinity(), then we need to do something else - use
    // the defaults that we calculated from issuing cpuid without binding
    // to each proc.
    //
    if (! KMP_AFFINITY_CAPABLE()) {
        //
        // Hack to try and infer the machine topology using only the data
        // available from cpuid on the current thread, and __kmp_xproc.
        //
        KMP_ASSERT(__kmp_affinity_type == affinity_none);

        //
        // Get an upper bound on the number of threads per package using
        // cpuid(1).
        //
        // On some OS/chps combinations where HT is supported by the chip
        // but is disabled, this value will be 2 on a single core chip.
        // Usually, it will be 2 if HT is enabled and 1 if HT is disabled.
        //
        __kmp_x86_cpuid(1, 0, &buf);
        int maxThreadsPerPkg = (buf.ebx >> 16) & 0xff;
        if (maxThreadsPerPkg == 0) {
            maxThreadsPerPkg = 1;
        }

        //
        // The num cores per pkg comes from cpuid(4).
        // 1 must be added to the encoded value.
        //
        // The author of cpu_count.cpp treated this only an upper bound
        // on the number of cores, but I haven't seen any cases where it
        // was greater than the actual number of cores, so we will treat
        // it as exact in this block of code.
        //
        // First, we need to check if cpuid(4) is supported on this chip.
        // To see if cpuid(n) is supported, issue cpuid(0) and check if eax
        // has the value n or greater.
        //
        __kmp_x86_cpuid(0, 0, &buf);
        if (buf.eax >= 4) {
            __kmp_x86_cpuid(4, 0, &buf);
            nCoresPerPkg = ((buf.eax >> 26) & 0x3f) + 1;
        }
        else {
            nCoresPerPkg = 1;
        }

        //
        // There is no way to reliably tell if HT is enabled without issuing
        // the cpuid instruction from every thread, can correlating the cpuid
        // info, so if the machine is not affinity capable, we assume that HT
        // is off.  We have seen quite a few machines where maxThreadsPerPkg
        // is 2, yet the machine does not support HT.
        //
        // - Older OSes are usually found on machines with older chips, which
        //   do not support HT.
        //
        // - The performance penalty for mistakenly identifying a machine as
        //   HT when it isn't (which results in blocktime being incorrecly set
        //   to 0) is greater than the penalty when for mistakenly identifying
        //   a machine as being 1 thread/core when it is really HT enabled
        //   (which results in blocktime being incorrectly set to a positive
        //   value).
        //
        __kmp_ncores = __kmp_xproc;
        nPackages = (__kmp_xproc + nCoresPerPkg - 1) / nCoresPerPkg;
        __kmp_nThreadsPerCore = 1;
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffNotCapableUseLocCpuid, "KMP_AFFINITY");
            KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
            if (__kmp_affinity_uniform_topology()) {
                KMP_INFORM(Uniform, "KMP_AFFINITY");
            } else {
                KMP_INFORM(NonUniform, "KMP_AFFINITY");
            }
            KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
              __kmp_nThreadsPerCore, __kmp_ncores);
        }
        return 0;
    }

    //
    //
    // From here on, we can assume that it is safe to call
    // __kmp_get_system_affinity() and __kmp_set_system_affinity(),
    // even if __kmp_affinity_type = affinity_none.
    //

    //
    // Save the affinity mask for the current thread.
    //
    kmp_affin_mask_t *oldMask;
    KMP_CPU_ALLOC(oldMask);
    KMP_ASSERT(oldMask != NULL);
    __kmp_get_system_affinity(oldMask, TRUE);

    //
    // Run through each of the available contexts, binding the current thread
    // to it, and obtaining the pertinent information using the cpuid instr.
    //
    // The relevant information is:
    //
    // Apic Id: Bits 24:31 of ebx after issuing cpuid(1) - each thread context
    //    has a uniqie Apic Id, which is of the form pkg# : core# : thread#.
    //
    // Max Threads Per Pkg: Bits 16:23 of ebx after issuing cpuid(1).  The
    //    value of this field determines the width of the core# + thread#
    //    fields in the Apic Id.  It is also an upper bound on the number
    //    of threads per package, but it has been verified that situations
    //    happen were it is not exact.  In particular, on certain OS/chip
    //    combinations where Intel(R) Hyper-Threading Technology is supported
    //    by the chip but has
    //    been disabled, the value of this field will be 2 (for a single core
    //    chip).  On other OS/chip combinations supporting
    //    Intel(R) Hyper-Threading Technology, the value of
    //    this field will be 1 when Intel(R) Hyper-Threading Technology is
    //    disabled and 2 when it is enabled.
    //
    // Max Cores Per Pkg:  Bits 26:31 of eax after issuing cpuid(4).  The
    //    value of this field (+1) determines the width of the core# field in
    //    the Apic Id.  The comments in "cpucount.cpp" say that this value is
    //    an upper bound, but the IA-32 architecture manual says that it is
    //    exactly the number of cores per package, and I haven't seen any
    //    case where it wasn't.
    //
    // From this information, deduce the package Id, core Id, and thread Id,
    // and set the corresponding fields in the apicThreadInfo struct.
    //
    unsigned i;
    apicThreadInfo *threadInfo = (apicThreadInfo *)__kmp_allocate(
      __kmp_avail_proc * sizeof(apicThreadInfo));
    unsigned nApics = 0;
    for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
        //
        // Skip this proc if it is not included in the machine model.
        //
        if (! KMP_CPU_ISSET(i, fullMask)) {
            continue;
        }
        KMP_DEBUG_ASSERT((int)nApics < __kmp_avail_proc);

        __kmp_affinity_bind_thread(i);
        threadInfo[nApics].osId = i;

        //
        // The apic id and max threads per pkg come from cpuid(1).
        //
        __kmp_x86_cpuid(1, 0, &buf);
        if (! (buf.edx >> 9) & 1) {
            __kmp_set_system_affinity(oldMask, TRUE);
            __kmp_free(threadInfo);
            KMP_CPU_FREE(oldMask);
            *msg_id = kmp_i18n_str_ApicNotPresent;
            return -1;
        }
        threadInfo[nApics].apicId = (buf.ebx >> 24) & 0xff;
        threadInfo[nApics].maxThreadsPerPkg = (buf.ebx >> 16) & 0xff;
        if (threadInfo[nApics].maxThreadsPerPkg == 0) {
            threadInfo[nApics].maxThreadsPerPkg = 1;
        }

        //
        // Max cores per pkg comes from cpuid(4).
        // 1 must be added to the encoded value.
        //
        // First, we need to check if cpuid(4) is supported on this chip.
        // To see if cpuid(n) is supported, issue cpuid(0) and check if eax
        // has the value n or greater.
        //
        __kmp_x86_cpuid(0, 0, &buf);
        if (buf.eax >= 4) {
            __kmp_x86_cpuid(4, 0, &buf);
            threadInfo[nApics].maxCoresPerPkg = ((buf.eax >> 26) & 0x3f) + 1;
        }
        else {
            threadInfo[nApics].maxCoresPerPkg = 1;
        }

        //
        // Infer the pkgId / coreId / threadId using only the info
        // obtained locally.
        //
        int widthCT = __kmp_cpuid_mask_width(
          threadInfo[nApics].maxThreadsPerPkg);
        threadInfo[nApics].pkgId = threadInfo[nApics].apicId >> widthCT;

        int widthC = __kmp_cpuid_mask_width(
          threadInfo[nApics].maxCoresPerPkg);
        int widthT = widthCT - widthC;
        if (widthT < 0) {
            //
            // I've never seen this one happen, but I suppose it could, if
            // the cpuid instruction on a chip was really screwed up.
            // Make sure to restore the affinity mask before the tail call.
            //
            __kmp_set_system_affinity(oldMask, TRUE);
            __kmp_free(threadInfo);
            KMP_CPU_FREE(oldMask);
            *msg_id = kmp_i18n_str_InvalidCpuidInfo;
            return -1;
        }

        int maskC = (1 << widthC) - 1;
        threadInfo[nApics].coreId = (threadInfo[nApics].apicId >> widthT)
          &maskC;

        int maskT = (1 << widthT) - 1;
        threadInfo[nApics].threadId = threadInfo[nApics].apicId &maskT;

        nApics++;
    }

    //
    // We've collected all the info we need.
    // Restore the old affinity mask for this thread.
    //
    __kmp_set_system_affinity(oldMask, TRUE);

    //
    // If there's only one thread context to bind to, form an Address object
    // with depth 1 and return immediately (or, if affinity is off, set
    // address2os to NULL and return).
    //
    // If it is configured to omit the package level when there is only a
    // single package, the logic at the end of this routine won't work if
    // there is only a single thread - it would try to form an Address
    // object with depth 0.
    //
    KMP_ASSERT(nApics > 0);
    if (nApics == 1) {
        __kmp_ncores = nPackages = 1;
        __kmp_nThreadsPerCore = nCoresPerPkg = 1;
        if (__kmp_affinity_verbose) {
            char buf[KMP_AFFIN_MASK_PRINT_LEN];
            __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, oldMask);

            KMP_INFORM(AffUseGlobCpuid, "KMP_AFFINITY");
            if (__kmp_affinity_respect_mask) {
                KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
            } else {
                KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
            }
            KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
            KMP_INFORM(Uniform, "KMP_AFFINITY");
            KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
              __kmp_nThreadsPerCore, __kmp_ncores);
        }

        if (__kmp_affinity_type == affinity_none) {
            __kmp_free(threadInfo);
            KMP_CPU_FREE(oldMask);
            return 0;
        }

        *address2os = (AddrUnsPair*)__kmp_allocate(sizeof(AddrUnsPair));
        Address addr(1);
        addr.labels[0] = threadInfo[0].pkgId;
        (*address2os)[0] = AddrUnsPair(addr, threadInfo[0].osId);

        if (__kmp_affinity_gran_levels < 0) {
            __kmp_affinity_gran_levels = 0;
        }

        if (__kmp_affinity_verbose) {
            __kmp_affinity_print_topology(*address2os, 1, 1, 0, -1, -1);
        }

        __kmp_free(threadInfo);
        KMP_CPU_FREE(oldMask);
        return 1;
    }

    //
    // Sort the threadInfo table by physical Id.
    //
    qsort(threadInfo, nApics, sizeof(*threadInfo),
      __kmp_affinity_cmp_apicThreadInfo_phys_id);

    //
    // The table is now sorted by pkgId / coreId / threadId, but we really
    // don't know the radix of any of the fields.  pkgId's may be sparsely
    // assigned among the chips on a system.  Although coreId's are usually
    // assigned [0 .. coresPerPkg-1] and threadId's are usually assigned
    // [0..threadsPerCore-1], we don't want to make any such assumptions.
    //
    // For that matter, we don't know what coresPerPkg and threadsPerCore
    // (or the total # packages) are at this point - we want to determine
    // that now.  We only have an upper bound on the first two figures.
    //
    // We also perform a consistency check at this point: the values returned
    // by the cpuid instruction for any thread bound to a given package had
    // better return the same info for maxThreadsPerPkg and maxCoresPerPkg.
    //
    nPackages = 1;
    nCoresPerPkg = 1;
    __kmp_nThreadsPerCore = 1;
    unsigned nCores = 1;

    unsigned pkgCt = 1;                         // to determine radii
    unsigned lastPkgId = threadInfo[0].pkgId;
    unsigned coreCt = 1;
    unsigned lastCoreId = threadInfo[0].coreId;
    unsigned threadCt = 1;
    unsigned lastThreadId = threadInfo[0].threadId;

                                                // intra-pkg consist checks
    unsigned prevMaxCoresPerPkg = threadInfo[0].maxCoresPerPkg;
    unsigned prevMaxThreadsPerPkg = threadInfo[0].maxThreadsPerPkg;

    for (i = 1; i < nApics; i++) {
        if (threadInfo[i].pkgId != lastPkgId) {
            nCores++;
            pkgCt++;
            lastPkgId = threadInfo[i].pkgId;
            if ((int)coreCt > nCoresPerPkg) nCoresPerPkg = coreCt;
            coreCt = 1;
            lastCoreId = threadInfo[i].coreId;
            if ((int)threadCt > __kmp_nThreadsPerCore) __kmp_nThreadsPerCore = threadCt;
            threadCt = 1;
            lastThreadId = threadInfo[i].threadId;

            //
            // This is a different package, so go on to the next iteration
            // without doing any consistency checks.  Reset the consistency
            // check vars, though.
            //
            prevMaxCoresPerPkg = threadInfo[i].maxCoresPerPkg;
            prevMaxThreadsPerPkg = threadInfo[i].maxThreadsPerPkg;
            continue;
        }

        if (threadInfo[i].coreId != lastCoreId) {
            nCores++;
            coreCt++;
            lastCoreId = threadInfo[i].coreId;
            if ((int)threadCt > __kmp_nThreadsPerCore) __kmp_nThreadsPerCore = threadCt;
            threadCt = 1;
            lastThreadId = threadInfo[i].threadId;
        }
        else if (threadInfo[i].threadId != lastThreadId) {
            threadCt++;
            lastThreadId = threadInfo[i].threadId;
        }
        else {
            __kmp_free(threadInfo);
            KMP_CPU_FREE(oldMask);
            *msg_id = kmp_i18n_str_LegacyApicIDsNotUnique;
            return -1;
        }

        //
        // Check to make certain that the maxCoresPerPkg and maxThreadsPerPkg
        // fields agree between all the threads bounds to a given package.
        //
        if ((prevMaxCoresPerPkg != threadInfo[i].maxCoresPerPkg)
          || (prevMaxThreadsPerPkg != threadInfo[i].maxThreadsPerPkg)) {
            __kmp_free(threadInfo);
            KMP_CPU_FREE(oldMask);
            *msg_id = kmp_i18n_str_InconsistentCpuidInfo;
            return -1;
        }
    }
    nPackages = pkgCt;
    if ((int)coreCt > nCoresPerPkg) nCoresPerPkg = coreCt;
    if ((int)threadCt > __kmp_nThreadsPerCore) __kmp_nThreadsPerCore = threadCt;

    //
    // When affinity is off, this routine will still be called to set
    // __kmp_ncores, as well as __kmp_nThreadsPerCore,
    // nCoresPerPkg, & nPackages.  Make sure all these vars are set
    // correctly, and return now if affinity is not enabled.
    //
    __kmp_ncores = nCores;
    if (__kmp_affinity_verbose) {
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, oldMask);

        KMP_INFORM(AffUseGlobCpuid, "KMP_AFFINITY");
        if (__kmp_affinity_respect_mask) {
            KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
        } else {
            KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
        }
        KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
        if (__kmp_affinity_uniform_topology()) {
            KMP_INFORM(Uniform, "KMP_AFFINITY");
        } else {
            KMP_INFORM(NonUniform, "KMP_AFFINITY");
        }
        KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
          __kmp_nThreadsPerCore, __kmp_ncores);

    }

    if (__kmp_affinity_type == affinity_none) {
        __kmp_free(threadInfo);
        KMP_CPU_FREE(oldMask);
        return 0;
    }

    //
    // Now that we've determined the number of packages, the number of cores
    // per package, and the number of threads per core, we can construct the
    // data structure that is to be returned.
    //
    int pkgLevel = 0;
    int coreLevel = (nCoresPerPkg <= 1) ? -1 : 1;
    int threadLevel = (__kmp_nThreadsPerCore <= 1) ? -1 : ((coreLevel >= 0) ? 2 : 1);
    unsigned depth = (pkgLevel >= 0) + (coreLevel >= 0) + (threadLevel >= 0);

    KMP_ASSERT(depth > 0);
    *address2os = (AddrUnsPair*)__kmp_allocate(sizeof(AddrUnsPair) * nApics);

    for (i = 0; i < nApics; ++i) {
        Address addr(depth);
        unsigned os = threadInfo[i].osId;
        int d = 0;

        if (pkgLevel >= 0) {
            addr.labels[d++] = threadInfo[i].pkgId;
        }
        if (coreLevel >= 0) {
            addr.labels[d++] = threadInfo[i].coreId;
        }
        if (threadLevel >= 0) {
            addr.labels[d++] = threadInfo[i].threadId;
        }
        (*address2os)[i] = AddrUnsPair(addr, os);
    }

    if (__kmp_affinity_gran_levels < 0) {
        //
        // Set the granularity level based on what levels are modeled
        // in the machine topology map.
        //
        __kmp_affinity_gran_levels = 0;
        if ((threadLevel >= 0)
          && (__kmp_affinity_gran > affinity_gran_thread)) {
            __kmp_affinity_gran_levels++;
        }
        if ((coreLevel >= 0) && (__kmp_affinity_gran > affinity_gran_core)) {
            __kmp_affinity_gran_levels++;
        }
        if ((pkgLevel >= 0) && (__kmp_affinity_gran > affinity_gran_package)) {
            __kmp_affinity_gran_levels++;
        }
    }

    if (__kmp_affinity_verbose) {
        __kmp_affinity_print_topology(*address2os, nApics, depth, pkgLevel,
          coreLevel, threadLevel);
    }

    __kmp_free(threadInfo);
    KMP_CPU_FREE(oldMask);
    return depth;
}


//
// Intel(R) microarchitecture code name Nehalem, Dunnington and later
// architectures support a newer interface for specifying the x2APIC Ids,
// based on cpuid leaf 11.
//
static int
__kmp_affinity_create_x2apicid_map(AddrUnsPair **address2os,
  kmp_i18n_id_t *const msg_id)
{
    kmp_cpuid buf;

    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    //
    // Check to see if cpuid leaf 11 is supported.
    //
    __kmp_x86_cpuid(0, 0, &buf);
    if (buf.eax < 11) {
        *msg_id = kmp_i18n_str_NoLeaf11Support;
        return -1;
    }
    __kmp_x86_cpuid(11, 0, &buf);
    if (buf.ebx == 0) {
        *msg_id = kmp_i18n_str_NoLeaf11Support;
        return -1;
    }

    //
    // Find the number of levels in the machine topology.  While we're at it,
    // get the default values for __kmp_nThreadsPerCore & nCoresPerPkg.  We will
    // try to get more accurate values later by explicitly counting them,
    // but get reasonable defaults now, in case we return early.
    //
    int level;
    int threadLevel = -1;
    int coreLevel = -1;
    int pkgLevel = -1;
    __kmp_nThreadsPerCore = nCoresPerPkg = nPackages = 1;

    for (level = 0;; level++) {
        if (level > 31) {
            //
            // FIXME: Hack for DPD200163180
            //
            // If level is big then something went wrong -> exiting
            //
            // There could actually be 32 valid levels in the machine topology,
            // but so far, the only machine we have seen which does not exit
            // this loop before iteration 32 has fubar x2APIC settings.
            //
            // For now, just reject this case based upon loop trip count.
            //
            *msg_id = kmp_i18n_str_InvalidCpuidInfo;
            return -1;
        }
        __kmp_x86_cpuid(11, level, &buf);
        if (buf.ebx == 0) {
            if (pkgLevel < 0) {
                //
                // Will infer nPackages from __kmp_xproc
                //
                pkgLevel = level;
                level++;
            }
            break;
        }
        int kind = (buf.ecx >> 8) & 0xff;
        if (kind == 1) {
            //
            // SMT level
            //
            threadLevel = level;
            coreLevel = -1;
            pkgLevel = -1;
            __kmp_nThreadsPerCore = buf.ebx & 0xff;
            if (__kmp_nThreadsPerCore == 0) {
                *msg_id = kmp_i18n_str_InvalidCpuidInfo;
                return -1;
            }
        }
        else if (kind == 2) {
            //
            // core level
            //
            coreLevel = level;
            pkgLevel = -1;
            nCoresPerPkg = buf.ebx & 0xff;
            if (nCoresPerPkg == 0) {
                *msg_id = kmp_i18n_str_InvalidCpuidInfo;
                return -1;
            }
        }
        else {
            if (level <= 0) {
                *msg_id = kmp_i18n_str_InvalidCpuidInfo;
                return -1;
            }
            if (pkgLevel >= 0) {
                continue;
            }
            pkgLevel = level;
            nPackages = buf.ebx & 0xff;
            if (nPackages == 0) {
                *msg_id = kmp_i18n_str_InvalidCpuidInfo;
                return -1;
            }
        }
    }
    int depth = level;

    //
    // In the above loop, "level" was counted from the finest level (usually
    // thread) to the coarsest.  The caller expects that we will place the
    // labels in (*address2os)[].first.labels[] in the inverse order, so
    // we need to invert the vars saying which level means what.
    //
    if (threadLevel >= 0) {
        threadLevel = depth - threadLevel - 1;
    }
    if (coreLevel >= 0) {
        coreLevel = depth - coreLevel - 1;
    }
    KMP_DEBUG_ASSERT(pkgLevel >= 0);
    pkgLevel = depth - pkgLevel - 1;

    //
    // The algorithm used starts by setting the affinity to each available
    // thread and retrieving info from the cpuid instruction, so if we are
    // not capable of calling __kmp_get_system_affinity() and
    // _kmp_get_system_affinity(), then we need to do something else - use
    // the defaults that we calculated from issuing cpuid without binding
    // to each proc.
    //
    if (! KMP_AFFINITY_CAPABLE())
    {
        //
        // Hack to try and infer the machine topology using only the data
        // available from cpuid on the current thread, and __kmp_xproc.
        //
        KMP_ASSERT(__kmp_affinity_type == affinity_none);

        __kmp_ncores = __kmp_xproc / __kmp_nThreadsPerCore;
        nPackages = (__kmp_xproc + nCoresPerPkg - 1) / nCoresPerPkg;
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffNotCapableUseLocCpuidL11, "KMP_AFFINITY");
            KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
            if (__kmp_affinity_uniform_topology()) {
                KMP_INFORM(Uniform, "KMP_AFFINITY");
            } else {
                KMP_INFORM(NonUniform, "KMP_AFFINITY");
            }
            KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
              __kmp_nThreadsPerCore, __kmp_ncores);
        }
        return 0;
    }

    //
    //
    // From here on, we can assume that it is safe to call
    // __kmp_get_system_affinity() and __kmp_set_system_affinity(),
    // even if __kmp_affinity_type = affinity_none.
    //

    //
    // Save the affinity mask for the current thread.
    //
    kmp_affin_mask_t *oldMask;
    KMP_CPU_ALLOC(oldMask);
    __kmp_get_system_affinity(oldMask, TRUE);

    //
    // Allocate the data structure to be returned.
    //
    AddrUnsPair *retval = (AddrUnsPair *)
      __kmp_allocate(sizeof(AddrUnsPair) * __kmp_avail_proc);

    //
    // Run through each of the available contexts, binding the current thread
    // to it, and obtaining the pertinent information using the cpuid instr.
    //
    unsigned int proc;
    int nApics = 0;
    for (proc = 0; proc < KMP_CPU_SETSIZE; ++proc) {
        //
        // Skip this proc if it is not included in the machine model.
        //
        if (! KMP_CPU_ISSET(proc, fullMask)) {
            continue;
        }
        KMP_DEBUG_ASSERT(nApics < __kmp_avail_proc);

        __kmp_affinity_bind_thread(proc);

        //
        // Extrach the labels for each level in the machine topology map
        // from the Apic ID.
        //
        Address addr(depth);
        int prev_shift = 0;

        for (level = 0; level < depth; level++) {
            __kmp_x86_cpuid(11, level, &buf);
            unsigned apicId = buf.edx;
            if (buf.ebx == 0) {
                if (level != depth - 1) {
                    KMP_CPU_FREE(oldMask);
                    *msg_id = kmp_i18n_str_InconsistentCpuidInfo;
                    return -1;
                }
                addr.labels[depth - level - 1] = apicId >> prev_shift;
                level++;
                break;
            }
            int shift = buf.eax & 0x1f;
            int mask = (1 << shift) - 1;
            addr.labels[depth - level - 1] = (apicId & mask) >> prev_shift;
            prev_shift = shift;
        }
        if (level != depth) {
            KMP_CPU_FREE(oldMask);
            *msg_id = kmp_i18n_str_InconsistentCpuidInfo;
            return -1;
        }

        retval[nApics] = AddrUnsPair(addr, proc);
        nApics++;
    }

    //
    // We've collected all the info we need.
    // Restore the old affinity mask for this thread.
    //
    __kmp_set_system_affinity(oldMask, TRUE);

    //
    // If there's only one thread context to bind to, return now.
    //
    KMP_ASSERT(nApics > 0);
    if (nApics == 1) {
        __kmp_ncores = nPackages = 1;
        __kmp_nThreadsPerCore = nCoresPerPkg = 1;
        if (__kmp_affinity_verbose) {
            char buf[KMP_AFFIN_MASK_PRINT_LEN];
            __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, oldMask);

            KMP_INFORM(AffUseGlobCpuidL11, "KMP_AFFINITY");
            if (__kmp_affinity_respect_mask) {
                KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
            } else {
                KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
            }
            KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
            KMP_INFORM(Uniform, "KMP_AFFINITY");
            KMP_INFORM(Topology, "KMP_AFFINITY", nPackages, nCoresPerPkg,
              __kmp_nThreadsPerCore, __kmp_ncores);
        }

        if (__kmp_affinity_type == affinity_none) {
            __kmp_free(retval);
            KMP_CPU_FREE(oldMask);
            return 0;
        }

        //
        // Form an Address object which only includes the package level.
        //
        Address addr(1);
        addr.labels[0] = retval[0].first.labels[pkgLevel];
        retval[0].first = addr;

        if (__kmp_affinity_gran_levels < 0) {
            __kmp_affinity_gran_levels = 0;
        }

        if (__kmp_affinity_verbose) {
            __kmp_affinity_print_topology(retval, 1, 1, 0, -1, -1);
        }

        *address2os = retval;
        KMP_CPU_FREE(oldMask);
        return 1;
    }

    //
    // Sort the table by physical Id.
    //
    qsort(retval, nApics, sizeof(*retval), __kmp_affinity_cmp_Address_labels);

    //
    // Find the radix at each of the levels.
    //
    unsigned *totals = (unsigned *)__kmp_allocate(depth * sizeof(unsigned));
    unsigned *counts = (unsigned *)__kmp_allocate(depth * sizeof(unsigned));
    unsigned *maxCt = (unsigned *)__kmp_allocate(depth * sizeof(unsigned));
    unsigned *last = (unsigned *)__kmp_allocate(depth * sizeof(unsigned));
    for (level = 0; level < depth; level++) {
        totals[level] = 1;
        maxCt[level] = 1;
        counts[level] = 1;
        last[level] = retval[0].first.labels[level];
    }

    //
    // From here on, the iteration variable "level" runs from the finest
    // level to the coarsest, i.e. we iterate forward through
    // (*address2os)[].first.labels[] - in the previous loops, we iterated
    // backwards.
    //
    for (proc = 1; (int)proc < nApics; proc++) {
        int level;
        for (level = 0; level < depth; level++) {
            if (retval[proc].first.labels[level] != last[level]) {
                int j;
                for (j = level + 1; j < depth; j++) {
                    totals[j]++;
                    counts[j] = 1;
                    // The line below causes printing incorrect topology information
                    // in case the max value for some level (maxCt[level]) is encountered earlier than
                    // some less value while going through the array.
                    // For example, let pkg0 has 4 cores and pkg1 has 2 cores. Then maxCt[1] == 2
                    // whereas it must be 4.
                    // TODO!!! Check if it can be commented safely
                    //maxCt[j] = 1;
                    last[j] = retval[proc].first.labels[j];
                }
                totals[level]++;
                counts[level]++;
                if (counts[level] > maxCt[level]) {
                    maxCt[level] = counts[level];
                }
                last[level] = retval[proc].first.labels[level];
                break;
            }
            else if (level == depth - 1) {
                __kmp_free(last);
                __kmp_free(maxCt);
                __kmp_free(counts);
                __kmp_free(totals);
                __kmp_free(retval);
                KMP_CPU_FREE(oldMask);
                *msg_id = kmp_i18n_str_x2ApicIDsNotUnique;
                return -1;
            }
        }
    }

    //
    // When affinity is off, this routine will still be called to set
    // __kmp_ncores, as well as __kmp_nThreadsPerCore,
    // nCoresPerPkg, & nPackages.  Make sure all these vars are set
    // correctly, and return if affinity is not enabled.
    //
    if (threadLevel >= 0) {
        __kmp_nThreadsPerCore = maxCt[threadLevel];
    }
    else {
        __kmp_nThreadsPerCore = 1;
    }
    nPackages = totals[pkgLevel];

    if (coreLevel >= 0) {
        __kmp_ncores = totals[coreLevel];
        nCoresPerPkg = maxCt[coreLevel];
    }
    else {
        __kmp_ncores = nPackages;
        nCoresPerPkg = 1;
    }

    //
    // Check to see if the machine topology is uniform
    //
    unsigned prod = maxCt[0];
    for (level = 1; level < depth; level++) {
       prod *= maxCt[level];
    }
    bool uniform = (prod == totals[level - 1]);

    //
    // Print the machine topology summary.
    //
    if (__kmp_affinity_verbose) {
        char mask[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(mask, KMP_AFFIN_MASK_PRINT_LEN, oldMask);

        KMP_INFORM(AffUseGlobCpuidL11, "KMP_AFFINITY");
        if (__kmp_affinity_respect_mask) {
            KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", mask);
        } else {
            KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", mask);
        }
        KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
        if (uniform) {
            KMP_INFORM(Uniform, "KMP_AFFINITY");
        } else {
            KMP_INFORM(NonUniform, "KMP_AFFINITY");
        }

        kmp_str_buf_t buf;
        __kmp_str_buf_init(&buf);

        __kmp_str_buf_print(&buf, "%d", totals[0]);
        for (level = 1; level <= pkgLevel; level++) {
            __kmp_str_buf_print(&buf, " x %d", maxCt[level]);
        }
        KMP_INFORM(TopologyExtra, "KMP_AFFINITY", buf.str, nCoresPerPkg,
          __kmp_nThreadsPerCore, __kmp_ncores);

        __kmp_str_buf_free(&buf);
    }

    if (__kmp_affinity_type == affinity_none) {
        __kmp_free(last);
        __kmp_free(maxCt);
        __kmp_free(counts);
        __kmp_free(totals);
        __kmp_free(retval);
        KMP_CPU_FREE(oldMask);
        return 0;
    }

    //
    // Find any levels with radiix 1, and remove them from the map
    // (except for the package level).
    //
    int new_depth = 0;
    for (level = 0; level < depth; level++) {
        if ((maxCt[level] == 1) && (level != pkgLevel)) {
           continue;
        }
        new_depth++;
    }

    //
    // If we are removing any levels, allocate a new vector to return,
    // and copy the relevant information to it.
    //
    if (new_depth != depth) {
        AddrUnsPair *new_retval = (AddrUnsPair *)__kmp_allocate(
          sizeof(AddrUnsPair) * nApics);
        for (proc = 0; (int)proc < nApics; proc++) {
            Address addr(new_depth);
            new_retval[proc] = AddrUnsPair(addr, retval[proc].second);
        }
        int new_level = 0;
        int newPkgLevel = -1;
        int newCoreLevel = -1;
        int newThreadLevel = -1;
        int i;
        for (level = 0; level < depth; level++) {
            if ((maxCt[level] == 1)
              && (level != pkgLevel)) {
                //
                // Remove this level. Never remove the package level
                //
                continue;
            }
            if (level == pkgLevel) {
                newPkgLevel = level;
            }
            if (level == coreLevel) {
                newCoreLevel = level;
            }
            if (level == threadLevel) {
                newThreadLevel = level;
            }
            for (proc = 0; (int)proc < nApics; proc++) {
                new_retval[proc].first.labels[new_level]
                  = retval[proc].first.labels[level];
            }
            new_level++;
        }

        __kmp_free(retval);
        retval = new_retval;
        depth = new_depth;
        pkgLevel = newPkgLevel;
        coreLevel = newCoreLevel;
        threadLevel = newThreadLevel;
    }

    if (__kmp_affinity_gran_levels < 0) {
        //
        // Set the granularity level based on what levels are modeled
        // in the machine topology map.
        //
        __kmp_affinity_gran_levels = 0;
        if ((threadLevel >= 0) && (__kmp_affinity_gran > affinity_gran_thread)) {
            __kmp_affinity_gran_levels++;
        }
        if ((coreLevel >= 0) && (__kmp_affinity_gran > affinity_gran_core)) {
            __kmp_affinity_gran_levels++;
        }
        if (__kmp_affinity_gran > affinity_gran_package) {
            __kmp_affinity_gran_levels++;
        }
    }

    if (__kmp_affinity_verbose) {
        __kmp_affinity_print_topology(retval, nApics, depth, pkgLevel,
          coreLevel, threadLevel);
    }

    __kmp_free(last);
    __kmp_free(maxCt);
    __kmp_free(counts);
    __kmp_free(totals);
    KMP_CPU_FREE(oldMask);
    *address2os = retval;
    return depth;
}


# endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */


#define osIdIndex       0
#define threadIdIndex   1
#define coreIdIndex     2
#define pkgIdIndex      3
#define nodeIdIndex     4

typedef unsigned *ProcCpuInfo;
static unsigned maxIndex = pkgIdIndex;


static int
__kmp_affinity_cmp_ProcCpuInfo_os_id(const void *a, const void *b)
{
    const unsigned *aa = (const unsigned *)a;
    const unsigned *bb = (const unsigned *)b;
    if (aa[osIdIndex] < bb[osIdIndex]) return -1;
    if (aa[osIdIndex] > bb[osIdIndex]) return 1;
    return 0;
};


static int
__kmp_affinity_cmp_ProcCpuInfo_phys_id(const void *a, const void *b)
{
    unsigned i;
    const unsigned *aa = *((const unsigned **)a);
    const unsigned *bb = *((const unsigned **)b);
    for (i = maxIndex; ; i--) {
        if (aa[i] < bb[i]) return -1;
        if (aa[i] > bb[i]) return 1;
        if (i == osIdIndex) break;
    }
    return 0;
}


//
// Parse /proc/cpuinfo (or an alternate file in the same format) to obtain the
// affinity map.
//
static int
__kmp_affinity_create_cpuinfo_map(AddrUnsPair **address2os, int *line,
  kmp_i18n_id_t *const msg_id, FILE *f)
{
    *address2os = NULL;
    *msg_id = kmp_i18n_null;

    //
    // Scan of the file, and count the number of "processor" (osId) fields,
    // and find the highest value of <n> for a node_<n> field.
    //
    char buf[256];
    unsigned num_records = 0;
    while (! feof(f)) {
        buf[sizeof(buf) - 1] = 1;
        if (! fgets(buf, sizeof(buf), f)) {
            //
            // Read errors presumably because of EOF
            //
            break;
        }

        char s1[] = "processor";
        if (strncmp(buf, s1, sizeof(s1) - 1) == 0) {
            num_records++;
            continue;
        }

        //
        // FIXME - this will match "node_<n> <garbage>"
        //
        unsigned level;
        if (KMP_SSCANF(buf, "node_%d id", &level) == 1) {
            if (nodeIdIndex + level >= maxIndex) {
                maxIndex = nodeIdIndex + level;
            }
            continue;
        }
    }

    //
    // Check for empty file / no valid processor records, or too many.
    // The number of records can't exceed the number of valid bits in the
    // affinity mask.
    //
    if (num_records == 0) {
        *line = 0;
        *msg_id = kmp_i18n_str_NoProcRecords;
        return -1;
    }
    if (num_records > (unsigned)__kmp_xproc) {
        *line = 0;
        *msg_id = kmp_i18n_str_TooManyProcRecords;
        return -1;
    }

    //
    // Set the file pointer back to the begginning, so that we can scan the
    // file again, this time performing a full parse of the data.
    // Allocate a vector of ProcCpuInfo object, where we will place the data.
    // Adding an extra element at the end allows us to remove a lot of extra
    // checks for termination conditions.
    //
    if (fseek(f, 0, SEEK_SET) != 0) {
        *line = 0;
        *msg_id = kmp_i18n_str_CantRewindCpuinfo;
        return -1;
    }

    //
    // Allocate the array of records to store the proc info in.  The dummy
    // element at the end makes the logic in filling them out easier to code.
    //
    unsigned **threadInfo = (unsigned **)__kmp_allocate((num_records + 1)
      * sizeof(unsigned *));
    unsigned i;
    for (i = 0; i <= num_records; i++) {
        threadInfo[i] = (unsigned *)__kmp_allocate((maxIndex + 1)
          * sizeof(unsigned));
    }

#define CLEANUP_THREAD_INFO \
    for (i = 0; i <= num_records; i++) {                                \
        __kmp_free(threadInfo[i]);                                      \
    }                                                                   \
    __kmp_free(threadInfo);

    //
    // A value of UINT_MAX means that we didn't find the field
    //
    unsigned __index;

#define INIT_PROC_INFO(p) \
    for (__index = 0; __index <= maxIndex; __index++) {                 \
        (p)[__index] = UINT_MAX;                                        \
    }

    for (i = 0; i <= num_records; i++) {
        INIT_PROC_INFO(threadInfo[i]);
    }

    unsigned num_avail = 0;
    *line = 0;
    while (! feof(f)) {
        //
        // Create an inner scoping level, so that all the goto targets at the
        // end of the loop appear in an outer scoping level.  This avoids
        // warnings about jumping past an initialization to a target in the
        // same block.
        //
        {
            buf[sizeof(buf) - 1] = 1;
            bool long_line = false;
            if (! fgets(buf, sizeof(buf), f)) {
                //
                // Read errors presumably because of EOF
                //
                // If there is valid data in threadInfo[num_avail], then fake
                // a blank line in ensure that the last address gets parsed.
                //
                bool valid = false;
                for (i = 0; i <= maxIndex; i++) {
                    if (threadInfo[num_avail][i] != UINT_MAX) {
                        valid = true;
                    }
                }
                if (! valid) {
                    break;
                }
                buf[0] = 0;
            } else if (!buf[sizeof(buf) - 1]) {
                //
                // The line is longer than the buffer.  Set a flag and don't
                // emit an error if we were going to ignore the line, anyway.
                //
                long_line = true;

#define CHECK_LINE \
    if (long_line) {                                                    \
        CLEANUP_THREAD_INFO;                                            \
        *msg_id = kmp_i18n_str_LongLineCpuinfo;                         \
        return -1;                                                      \
    }
            }
            (*line)++;

            char s1[] = "processor";
            if (strncmp(buf, s1, sizeof(s1) - 1) == 0) {
                CHECK_LINE;
                char *p = strchr(buf + sizeof(s1) - 1, ':');
                unsigned val;
                if ((p == NULL) || (KMP_SSCANF(p + 1, "%u\n", &val) != 1)) goto no_val;
                if (threadInfo[num_avail][osIdIndex] != UINT_MAX) goto dup_field;
                threadInfo[num_avail][osIdIndex] = val;
#if KMP_OS_LINUX && USE_SYSFS_INFO
                char path[256];
                KMP_SNPRINTF(path, sizeof(path),
                    "/sys/devices/system/cpu/cpu%u/topology/physical_package_id",
                    threadInfo[num_avail][osIdIndex]);
                __kmp_read_from_file(path, "%u", &threadInfo[num_avail][pkgIdIndex]);

                KMP_SNPRINTF(path, sizeof(path),
                    "/sys/devices/system/cpu/cpu%u/topology/core_id",
                    threadInfo[num_avail][osIdIndex]);
                __kmp_read_from_file(path, "%u", &threadInfo[num_avail][coreIdIndex]);
                continue;
#else
            }
            char s2[] = "physical id";
            if (strncmp(buf, s2, sizeof(s2) - 1) == 0) {
                CHECK_LINE;
                char *p = strchr(buf + sizeof(s2) - 1, ':');
                unsigned val;
                if ((p == NULL) || (KMP_SSCANF(p + 1, "%u\n", &val) != 1)) goto no_val;
                if (threadInfo[num_avail][pkgIdIndex] != UINT_MAX) goto dup_field;
                threadInfo[num_avail][pkgIdIndex] = val;
                continue;
            }
            char s3[] = "core id";
            if (strncmp(buf, s3, sizeof(s3) - 1) == 0) {
                CHECK_LINE;
                char *p = strchr(buf + sizeof(s3) - 1, ':');
                unsigned val;
                if ((p == NULL) || (KMP_SSCANF(p + 1, "%u\n", &val) != 1)) goto no_val;
                if (threadInfo[num_avail][coreIdIndex] != UINT_MAX) goto dup_field;
                threadInfo[num_avail][coreIdIndex] = val;
                continue;
#endif // KMP_OS_LINUX && USE_SYSFS_INFO
            }
            char s4[] = "thread id";
            if (strncmp(buf, s4, sizeof(s4) - 1) == 0) {
                CHECK_LINE;
                char *p = strchr(buf + sizeof(s4) - 1, ':');
                unsigned val;
                if ((p == NULL) || (KMP_SSCANF(p + 1, "%u\n", &val) != 1)) goto no_val;
                if (threadInfo[num_avail][threadIdIndex] != UINT_MAX) goto dup_field;
                threadInfo[num_avail][threadIdIndex] = val;
                continue;
            }
            unsigned level;
            if (KMP_SSCANF(buf, "node_%d id", &level) == 1) {
                CHECK_LINE;
                char *p = strchr(buf + sizeof(s4) - 1, ':');
                unsigned val;
                if ((p == NULL) || (KMP_SSCANF(p + 1, "%u\n", &val) != 1)) goto no_val;
                KMP_ASSERT(nodeIdIndex + level <= maxIndex);
                if (threadInfo[num_avail][nodeIdIndex + level] != UINT_MAX) goto dup_field;
                threadInfo[num_avail][nodeIdIndex + level] = val;
                continue;
            }

            //
            // We didn't recognize the leading token on the line.
            // There are lots of leading tokens that we don't recognize -
            // if the line isn't empty, go on to the next line.
            //
            if ((*buf != 0) && (*buf != '\n')) {
                //
                // If the line is longer than the buffer, read characters
                // until we find a newline.
                //
                if (long_line) {
                    int ch;
                    while (((ch = fgetc(f)) != EOF) && (ch != '\n'));
                }
                continue;
            }

            //
            // A newline has signalled the end of the processor record.
            // Check that there aren't too many procs specified.
            //
            if ((int)num_avail == __kmp_xproc) {
                CLEANUP_THREAD_INFO;
                *msg_id = kmp_i18n_str_TooManyEntries;
                return -1;
            }

            //
            // Check for missing fields.  The osId field must be there, and we
            // currently require that the physical id field is specified, also.
            //
            if (threadInfo[num_avail][osIdIndex] == UINT_MAX) {
                CLEANUP_THREAD_INFO;
                *msg_id = kmp_i18n_str_MissingProcField;
                return -1;
            }
            if (threadInfo[0][pkgIdIndex] == UINT_MAX) {
                CLEANUP_THREAD_INFO;
                *msg_id = kmp_i18n_str_MissingPhysicalIDField;
                return -1;
            }

            //
            // Skip this proc if it is not included in the machine model.
            //
            if (! KMP_CPU_ISSET(threadInfo[num_avail][osIdIndex], fullMask)) {
                INIT_PROC_INFO(threadInfo[num_avail]);
                continue;
            }

            //
            // We have a successful parse of this proc's info.
            // Increment the counter, and prepare for the next proc.
            //
            num_avail++;
            KMP_ASSERT(num_avail <= num_records);
            INIT_PROC_INFO(threadInfo[num_avail]);
        }
        continue;

        no_val:
        CLEANUP_THREAD_INFO;
        *msg_id = kmp_i18n_str_MissingValCpuinfo;
        return -1;

        dup_field:
        CLEANUP_THREAD_INFO;
        *msg_id = kmp_i18n_str_DuplicateFieldCpuinfo;
        return -1;
    }
    *line = 0;

# if KMP_MIC && REDUCE_TEAM_SIZE
    unsigned teamSize = 0;
# endif // KMP_MIC && REDUCE_TEAM_SIZE

    // check for num_records == __kmp_xproc ???

    //
    // If there's only one thread context to bind to, form an Address object
    // with depth 1 and return immediately (or, if affinity is off, set
    // address2os to NULL and return).
    //
    // If it is configured to omit the package level when there is only a
    // single package, the logic at the end of this routine won't work if
    // there is only a single thread - it would try to form an Address
    // object with depth 0.
    //
    KMP_ASSERT(num_avail > 0);
    KMP_ASSERT(num_avail <= num_records);
    if (num_avail == 1) {
        __kmp_ncores = 1;
        __kmp_nThreadsPerCore = nCoresPerPkg = nPackages = 1;
        if (__kmp_affinity_verbose) {
            if (! KMP_AFFINITY_CAPABLE()) {
                KMP_INFORM(AffNotCapableUseCpuinfo, "KMP_AFFINITY");
                KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
                KMP_INFORM(Uniform, "KMP_AFFINITY");
            }
            else {
                char buf[KMP_AFFIN_MASK_PRINT_LEN];
                __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
                  fullMask);
                KMP_INFORM(AffCapableUseCpuinfo, "KMP_AFFINITY");
                if (__kmp_affinity_respect_mask) {
                    KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
                } else {
                    KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
                }
                KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
                KMP_INFORM(Uniform, "KMP_AFFINITY");
            }
            int index;
            kmp_str_buf_t buf;
            __kmp_str_buf_init(&buf);
            __kmp_str_buf_print(&buf, "1");
            for (index = maxIndex - 1; index > pkgIdIndex; index--) {
                __kmp_str_buf_print(&buf, " x 1");
            }
            KMP_INFORM(TopologyExtra, "KMP_AFFINITY", buf.str, 1, 1, 1);
            __kmp_str_buf_free(&buf);
        }

        if (__kmp_affinity_type == affinity_none) {
            CLEANUP_THREAD_INFO;
            return 0;
        }

        *address2os = (AddrUnsPair*)__kmp_allocate(sizeof(AddrUnsPair));
        Address addr(1);
        addr.labels[0] = threadInfo[0][pkgIdIndex];
        (*address2os)[0] = AddrUnsPair(addr, threadInfo[0][osIdIndex]);

        if (__kmp_affinity_gran_levels < 0) {
            __kmp_affinity_gran_levels = 0;
        }

        if (__kmp_affinity_verbose) {
            __kmp_affinity_print_topology(*address2os, 1, 1, 0, -1, -1);
        }

        CLEANUP_THREAD_INFO;
        return 1;
    }

    //
    // Sort the threadInfo table by physical Id.
    //
    qsort(threadInfo, num_avail, sizeof(*threadInfo),
      __kmp_affinity_cmp_ProcCpuInfo_phys_id);

    //
    // The table is now sorted by pkgId / coreId / threadId, but we really
    // don't know the radix of any of the fields.  pkgId's may be sparsely
    // assigned among the chips on a system.  Although coreId's are usually
    // assigned [0 .. coresPerPkg-1] and threadId's are usually assigned
    // [0..threadsPerCore-1], we don't want to make any such assumptions.
    //
    // For that matter, we don't know what coresPerPkg and threadsPerCore
    // (or the total # packages) are at this point - we want to determine
    // that now.  We only have an upper bound on the first two figures.
    //
    unsigned *counts = (unsigned *)__kmp_allocate((maxIndex + 1)
      * sizeof(unsigned));
    unsigned *maxCt = (unsigned *)__kmp_allocate((maxIndex + 1)
      * sizeof(unsigned));
    unsigned *totals = (unsigned *)__kmp_allocate((maxIndex + 1)
      * sizeof(unsigned));
    unsigned *lastId = (unsigned *)__kmp_allocate((maxIndex + 1)
      * sizeof(unsigned));

    bool assign_thread_ids = false;
    unsigned threadIdCt;
    unsigned index;

    restart_radix_check:
    threadIdCt = 0;

    //
    // Initialize the counter arrays with data from threadInfo[0].
    //
    if (assign_thread_ids) {
        if (threadInfo[0][threadIdIndex] == UINT_MAX) {
            threadInfo[0][threadIdIndex] = threadIdCt++;
        }
        else if (threadIdCt <= threadInfo[0][threadIdIndex]) {
            threadIdCt = threadInfo[0][threadIdIndex] + 1;
        }
    }
    for (index = 0; index <= maxIndex; index++) {
        counts[index] = 1;
        maxCt[index] = 1;
        totals[index] = 1;
        lastId[index] = threadInfo[0][index];;
    }

    //
    // Run through the rest of the OS procs.
    //
    for (i = 1; i < num_avail; i++) {
        //
        // Find the most significant index whose id differs
        // from the id for the previous OS proc.
        //
        for (index = maxIndex; index >= threadIdIndex; index--) {
            if (assign_thread_ids && (index == threadIdIndex)) {
                //
                // Auto-assign the thread id field if it wasn't specified.
                //
                if (threadInfo[i][threadIdIndex] == UINT_MAX) {
                    threadInfo[i][threadIdIndex] = threadIdCt++;
                }

                //
                // Aparrently the thread id field was specified for some
                // entries and not others.  Start the thread id counter
                // off at the next higher thread id.
                //
                else if (threadIdCt <= threadInfo[i][threadIdIndex]) {
                    threadIdCt = threadInfo[i][threadIdIndex] + 1;
                }
            }
            if (threadInfo[i][index] != lastId[index]) {
                //
                // Run through all indices which are less significant,
                // and reset the counts to 1.
                //
                // At all levels up to and including index, we need to
                // increment the totals and record the last id.
                //
                unsigned index2;
                for (index2 = threadIdIndex; index2 < index; index2++) {
                    totals[index2]++;
                    if (counts[index2] > maxCt[index2]) {
                        maxCt[index2] = counts[index2];
                    }
                    counts[index2] = 1;
                    lastId[index2] = threadInfo[i][index2];
                }
                counts[index]++;
                totals[index]++;
                lastId[index] = threadInfo[i][index];

                if (assign_thread_ids && (index > threadIdIndex)) {

# if KMP_MIC && REDUCE_TEAM_SIZE
                    //
                    // The default team size is the total #threads in the machine
                    // minus 1 thread for every core that has 3 or more threads.
                    //
                    teamSize += ( threadIdCt <= 2 ) ? ( threadIdCt ) : ( threadIdCt - 1 );
# endif // KMP_MIC && REDUCE_TEAM_SIZE

                    //
                    // Restart the thread counter, as we are on a new core.
                    //
                    threadIdCt = 0;

                    //
                    // Auto-assign the thread id field if it wasn't specified.
                    //
                    if (threadInfo[i][threadIdIndex] == UINT_MAX) {
                        threadInfo[i][threadIdIndex] = threadIdCt++;
                    }

                    //
                    // Aparrently the thread id field was specified for some
                    // entries and not others.  Start the thread id counter
                    // off at the next higher thread id.
                    //
                    else if (threadIdCt <= threadInfo[i][threadIdIndex]) {
                        threadIdCt = threadInfo[i][threadIdIndex] + 1;
                    }
                }
                break;
            }
        }
        if (index < threadIdIndex) {
            //
            // If thread ids were specified, it is an error if they are not
            // unique.  Also, check that we waven't already restarted the
            // loop (to be safe - shouldn't need to).
            //
            if ((threadInfo[i][threadIdIndex] != UINT_MAX)
              || assign_thread_ids) {
                __kmp_free(lastId);
                __kmp_free(totals);
                __kmp_free(maxCt);
                __kmp_free(counts);
                CLEANUP_THREAD_INFO;
                *msg_id = kmp_i18n_str_PhysicalIDsNotUnique;
                return -1;
            }

            //
            // If the thread ids were not specified and we see entries
            // entries that are duplicates, start the loop over and
            // assign the thread ids manually.
            //
            assign_thread_ids = true;
            goto restart_radix_check;
        }
    }

# if KMP_MIC && REDUCE_TEAM_SIZE
    //
    // The default team size is the total #threads in the machine
    // minus 1 thread for every core that has 3 or more threads.
    //
    teamSize += ( threadIdCt <= 2 ) ? ( threadIdCt ) : ( threadIdCt - 1 );
# endif // KMP_MIC && REDUCE_TEAM_SIZE

    for (index = threadIdIndex; index <= maxIndex; index++) {
        if (counts[index] > maxCt[index]) {
            maxCt[index] = counts[index];
        }
    }

    __kmp_nThreadsPerCore = maxCt[threadIdIndex];
    nCoresPerPkg = maxCt[coreIdIndex];
    nPackages = totals[pkgIdIndex];

    //
    // Check to see if the machine topology is uniform
    //
    unsigned prod = totals[maxIndex];
    for (index = threadIdIndex; index < maxIndex; index++) {
       prod *= maxCt[index];
    }
    bool uniform = (prod == totals[threadIdIndex]);

    //
    // When affinity is off, this routine will still be called to set
    // __kmp_ncores, as well as __kmp_nThreadsPerCore,
    // nCoresPerPkg, & nPackages.  Make sure all these vars are set
    // correctly, and return now if affinity is not enabled.
    //
    __kmp_ncores = totals[coreIdIndex];

    if (__kmp_affinity_verbose) {
        if (! KMP_AFFINITY_CAPABLE()) {
                KMP_INFORM(AffNotCapableUseCpuinfo, "KMP_AFFINITY");
                KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
                if (uniform) {
                    KMP_INFORM(Uniform, "KMP_AFFINITY");
                } else {
                    KMP_INFORM(NonUniform, "KMP_AFFINITY");
                }
        }
        else {
            char buf[KMP_AFFIN_MASK_PRINT_LEN];
            __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, fullMask);
                KMP_INFORM(AffCapableUseCpuinfo, "KMP_AFFINITY");
                if (__kmp_affinity_respect_mask) {
                    KMP_INFORM(InitOSProcSetRespect, "KMP_AFFINITY", buf);
                } else {
                    KMP_INFORM(InitOSProcSetNotRespect, "KMP_AFFINITY", buf);
                }
                KMP_INFORM(AvailableOSProc, "KMP_AFFINITY", __kmp_avail_proc);
                if (uniform) {
                    KMP_INFORM(Uniform, "KMP_AFFINITY");
                } else {
                    KMP_INFORM(NonUniform, "KMP_AFFINITY");
                }
        }
        kmp_str_buf_t buf;
        __kmp_str_buf_init(&buf);

        __kmp_str_buf_print(&buf, "%d", totals[maxIndex]);
        for (index = maxIndex - 1; index >= pkgIdIndex; index--) {
            __kmp_str_buf_print(&buf, " x %d", maxCt[index]);
        }
        KMP_INFORM(TopologyExtra, "KMP_AFFINITY", buf.str,  maxCt[coreIdIndex],
          maxCt[threadIdIndex], __kmp_ncores);

        __kmp_str_buf_free(&buf);
    }

# if KMP_MIC && REDUCE_TEAM_SIZE
    //
    // Set the default team size.
    //
    if ((__kmp_dflt_team_nth == 0) && (teamSize > 0)) {
        __kmp_dflt_team_nth = teamSize;
        KA_TRACE(20, ("__kmp_affinity_create_cpuinfo_map: setting __kmp_dflt_team_nth = %d\n",
          __kmp_dflt_team_nth));
    }
# endif // KMP_MIC && REDUCE_TEAM_SIZE

    if (__kmp_affinity_type == affinity_none) {
        __kmp_free(lastId);
        __kmp_free(totals);
        __kmp_free(maxCt);
        __kmp_free(counts);
        CLEANUP_THREAD_INFO;
        return 0;
    }

    //
    // Count the number of levels which have more nodes at that level than
    // at the parent's level (with there being an implicit root node of
    // the top level).  This is equivalent to saying that there is at least
    // one node at this level which has a sibling.  These levels are in the
    // map, and the package level is always in the map.
    //
    bool *inMap = (bool *)__kmp_allocate((maxIndex + 1) * sizeof(bool));
    int level = 0;
    for (index = threadIdIndex; index < maxIndex; index++) {
        KMP_ASSERT(totals[index] >= totals[index + 1]);
        inMap[index] = (totals[index] > totals[index + 1]);
    }
    inMap[maxIndex] = (totals[maxIndex] > 1);
    inMap[pkgIdIndex] = true;

    int depth = 0;
    for (index = threadIdIndex; index <= maxIndex; index++) {
        if (inMap[index]) {
            depth++;
        }
    }
    KMP_ASSERT(depth > 0);

    //
    // Construct the data structure that is to be returned.
    //
    *address2os = (AddrUnsPair*)
      __kmp_allocate(sizeof(AddrUnsPair) * num_avail);
    int pkgLevel = -1;
    int coreLevel = -1;
    int threadLevel = -1;

    for (i = 0; i < num_avail; ++i) {
        Address addr(depth);
        unsigned os = threadInfo[i][osIdIndex];
        int src_index;
        int dst_index = 0;

        for (src_index = maxIndex; src_index >= threadIdIndex; src_index--) {
            if (! inMap[src_index]) {
                continue;
            }
            addr.labels[dst_index] = threadInfo[i][src_index];
            if (src_index == pkgIdIndex) {
                pkgLevel = dst_index;
            }
            else if (src_index == coreIdIndex) {
                coreLevel = dst_index;
            }
            else if (src_index == threadIdIndex) {
                threadLevel = dst_index;
            }
            dst_index++;
        }
        (*address2os)[i] = AddrUnsPair(addr, os);
    }

    if (__kmp_affinity_gran_levels < 0) {
        //
        // Set the granularity level based on what levels are modeled
        // in the machine topology map.
        //
        unsigned src_index;
        __kmp_affinity_gran_levels = 0;
        for (src_index = threadIdIndex; src_index <= maxIndex; src_index++) {
            if (! inMap[src_index]) {
                continue;
            }
            switch (src_index) {
                case threadIdIndex:
                if (__kmp_affinity_gran > affinity_gran_thread) {
                    __kmp_affinity_gran_levels++;
                }

                break;
                case coreIdIndex:
                if (__kmp_affinity_gran > affinity_gran_core) {
                    __kmp_affinity_gran_levels++;
                }
                break;

                case pkgIdIndex:
                if (__kmp_affinity_gran > affinity_gran_package) {
                    __kmp_affinity_gran_levels++;
                }
                break;
            }
        }
    }

    if (__kmp_affinity_verbose) {
        __kmp_affinity_print_topology(*address2os, num_avail, depth, pkgLevel,
          coreLevel, threadLevel);
    }

    __kmp_free(inMap);
    __kmp_free(lastId);
    __kmp_free(totals);
    __kmp_free(maxCt);
    __kmp_free(counts);
    CLEANUP_THREAD_INFO;
    return depth;
}


//
// Create and return a table of affinity masks, indexed by OS thread ID.
// This routine handles OR'ing together all the affinity masks of threads
// that are sufficiently close, if granularity > fine.
//
static kmp_affin_mask_t *
__kmp_create_masks(unsigned *maxIndex, unsigned *numUnique,
  AddrUnsPair *address2os, unsigned numAddrs)
{
    //
    // First form a table of affinity masks in order of OS thread id.
    //
    unsigned depth;
    unsigned maxOsId;
    unsigned i;

    KMP_ASSERT(numAddrs > 0);
    depth = address2os[0].first.depth;

    maxOsId = 0;
    for (i = 0; i < numAddrs; i++) {
        unsigned osId = address2os[i].second;
        if (osId > maxOsId) {
            maxOsId = osId;
        }
    }
    kmp_affin_mask_t *osId2Mask = (kmp_affin_mask_t *)__kmp_allocate(
      (maxOsId + 1) * __kmp_affin_mask_size);

    //
    // Sort the address2os table according to physical order.  Doing so
    // will put all threads on the same core/package/node in consecutive
    // locations.
    //
    qsort(address2os, numAddrs, sizeof(*address2os),
      __kmp_affinity_cmp_Address_labels);

    KMP_ASSERT(__kmp_affinity_gran_levels >= 0);
    if (__kmp_affinity_verbose && (__kmp_affinity_gran_levels > 0)) {
        KMP_INFORM(ThreadsMigrate, "KMP_AFFINITY",  __kmp_affinity_gran_levels);
    }
    if (__kmp_affinity_gran_levels >= (int)depth) {
        if (__kmp_affinity_verbose || (__kmp_affinity_warnings
          && (__kmp_affinity_type != affinity_none))) {
            KMP_WARNING(AffThreadsMayMigrate);
        }
    }

    //
    // Run through the table, forming the masks for all threads on each
    // core.  Threads on the same core will have identical "Address"
    // objects, not considering the last level, which must be the thread
    // id.  All threads on a core will appear consecutively.
    //
    unsigned unique = 0;
    unsigned j = 0;                             // index of 1st thread on core
    unsigned leader = 0;
    Address *leaderAddr = &(address2os[0].first);
    kmp_affin_mask_t *sum
      = (kmp_affin_mask_t *)KMP_ALLOCA(__kmp_affin_mask_size);
    KMP_CPU_ZERO(sum);
    KMP_CPU_SET(address2os[0].second, sum);
    for (i = 1; i < numAddrs; i++) {
        //
        // If this thread is sufficiently close to the leader (within the
        // granularity setting), then set the bit for this os thread in the
        // affinity mask for this group, and go on to the next thread.
        //
        if (leaderAddr->isClose(address2os[i].first,
          __kmp_affinity_gran_levels)) {
            KMP_CPU_SET(address2os[i].second, sum);
            continue;
        }

        //
        // For every thread in this group, copy the mask to the thread's
        // entry in the osId2Mask table.  Mark the first address as a
        // leader.
        //
        for (; j < i; j++) {
            unsigned osId = address2os[j].second;
            KMP_DEBUG_ASSERT(osId <= maxOsId);
            kmp_affin_mask_t *mask = KMP_CPU_INDEX(osId2Mask, osId);
            KMP_CPU_COPY(mask, sum);
            address2os[j].first.leader = (j == leader);
        }
        unique++;

        //
        // Start a new mask.
        //
        leader = i;
        leaderAddr = &(address2os[i].first);
        KMP_CPU_ZERO(sum);
        KMP_CPU_SET(address2os[i].second, sum);
    }

    //
    // For every thread in last group, copy the mask to the thread's
    // entry in the osId2Mask table.
    //
    for (; j < i; j++) {
        unsigned osId = address2os[j].second;
        KMP_DEBUG_ASSERT(osId <= maxOsId);
        kmp_affin_mask_t *mask = KMP_CPU_INDEX(osId2Mask, osId);
        KMP_CPU_COPY(mask, sum);
        address2os[j].first.leader = (j == leader);
    }
    unique++;

    *maxIndex = maxOsId;
    *numUnique = unique;
    return osId2Mask;
}


//
// Stuff for the affinity proclist parsers.  It's easier to declare these vars
// as file-static than to try and pass them through the calling sequence of
// the recursive-descent OMP_PLACES parser.
//
static kmp_affin_mask_t *newMasks;
static int numNewMasks;
static int nextNewMask;

#define ADD_MASK(_mask) \
    {                                                                   \
        if (nextNewMask >= numNewMasks) {                               \
            numNewMasks *= 2;                                           \
            newMasks = (kmp_affin_mask_t *)KMP_INTERNAL_REALLOC(newMasks, \
              numNewMasks * __kmp_affin_mask_size);                     \
        }                                                               \
        KMP_CPU_COPY(KMP_CPU_INDEX(newMasks, nextNewMask), (_mask));    \
        nextNewMask++;                                                  \
    }

#define ADD_MASK_OSID(_osId,_osId2Mask,_maxOsId) \
    {                                                                   \
        if (((_osId) > _maxOsId) ||                                     \
          (! KMP_CPU_ISSET((_osId), KMP_CPU_INDEX((_osId2Mask), (_osId))))) { \
            if (__kmp_affinity_verbose || (__kmp_affinity_warnings      \
              && (__kmp_affinity_type != affinity_none))) {             \
                KMP_WARNING(AffIgnoreInvalidProcID, _osId);             \
            }                                                           \
        }                                                               \
        else {                                                          \
            ADD_MASK(KMP_CPU_INDEX(_osId2Mask, (_osId)));               \
        }                                                               \
    }


//
// Re-parse the proclist (for the explicit affinity type), and form the list
// of affinity newMasks indexed by gtid.
//
static void
__kmp_affinity_process_proclist(kmp_affin_mask_t **out_masks,
  unsigned int *out_numMasks, const char *proclist,
  kmp_affin_mask_t *osId2Mask, int maxOsId)
{
    const char *scan = proclist;
    const char *next = proclist;

    //
    // We use malloc() for the temporary mask vector,
    // so that we can use realloc() to extend it.
    //
    numNewMasks = 2;
    newMasks = (kmp_affin_mask_t *)KMP_INTERNAL_MALLOC(numNewMasks
      * __kmp_affin_mask_size);
    nextNewMask = 0;
    kmp_affin_mask_t *sumMask = (kmp_affin_mask_t *)__kmp_allocate(
      __kmp_affin_mask_size);
    int setSize = 0;

    for (;;) {
        int start, end, stride;

        SKIP_WS(scan);
        next = scan;
        if (*next == '\0') {
            break;
        }

        if (*next == '{') {
            int num;
            setSize = 0;
            next++;     // skip '{'
            SKIP_WS(next);
            scan = next;

            //
            // Read the first integer in the set.
            //
            KMP_ASSERT2((*next >= '0') && (*next <= '9'),
              "bad proclist");
            SKIP_DIGITS(next);
            num = __kmp_str_to_int(scan, *next);
            KMP_ASSERT2(num >= 0, "bad explicit proc list");

            //
            // Copy the mask for that osId to the sum (union) mask.
            //
            if ((num > maxOsId) ||
              (! KMP_CPU_ISSET(num, KMP_CPU_INDEX(osId2Mask, num)))) {
                if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                  && (__kmp_affinity_type != affinity_none))) {
                    KMP_WARNING(AffIgnoreInvalidProcID, num);
                }
                KMP_CPU_ZERO(sumMask);
            }
            else {
                KMP_CPU_COPY(sumMask, KMP_CPU_INDEX(osId2Mask, num));
                setSize = 1;
            }

            for (;;) {
                //
                // Check for end of set.
                //
                SKIP_WS(next);
                if (*next == '}') {
                    next++;     // skip '}'
                    break;
                }

                //
                // Skip optional comma.
                //
                if (*next == ',') {
                    next++;
                }
                SKIP_WS(next);

                //
                // Read the next integer in the set.
                //
                scan = next;
                KMP_ASSERT2((*next >= '0') && (*next <= '9'),
                  "bad explicit proc list");

                SKIP_DIGITS(next);
                num = __kmp_str_to_int(scan, *next);
                KMP_ASSERT2(num >= 0, "bad explicit proc list");

                //
                // Add the mask for that osId to the sum mask.
                //
                if ((num > maxOsId) ||
                  (! KMP_CPU_ISSET(num, KMP_CPU_INDEX(osId2Mask, num)))) {
                    if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                      && (__kmp_affinity_type != affinity_none))) {
                        KMP_WARNING(AffIgnoreInvalidProcID, num);
                    }
                }
                else {
                    KMP_CPU_UNION(sumMask, KMP_CPU_INDEX(osId2Mask, num));
                    setSize++;
                }
            }
            if (setSize > 0) {
                ADD_MASK(sumMask);
            }

            SKIP_WS(next);
            if (*next == ',') {
                next++;
            }
            scan = next;
            continue;
        }

        //
        // Read the first integer.
        //
        KMP_ASSERT2((*next >= '0') && (*next <= '9'), "bad explicit proc list");
        SKIP_DIGITS(next);
        start = __kmp_str_to_int(scan, *next);
        KMP_ASSERT2(start >= 0, "bad explicit proc list");
        SKIP_WS(next);

        //
        // If this isn't a range, then add a mask to the list and go on.
        //
        if (*next != '-') {
            ADD_MASK_OSID(start, osId2Mask, maxOsId);

            //
            // Skip optional comma.
            //
            if (*next == ',') {
                next++;
            }
            scan = next;
            continue;
        }

        //
        // This is a range.  Skip over the '-' and read in the 2nd int.
        //
        next++;         // skip '-'
        SKIP_WS(next);
        scan = next;
        KMP_ASSERT2((*next >= '0') && (*next <= '9'), "bad explicit proc list");
        SKIP_DIGITS(next);
        end = __kmp_str_to_int(scan, *next);
        KMP_ASSERT2(end >= 0, "bad explicit proc list");

        //
        // Check for a stride parameter
        //
        stride = 1;
        SKIP_WS(next);
        if (*next == ':') {
            //
            // A stride is specified.  Skip over the ':" and read the 3rd int.
            //
            int sign = +1;
            next++;         // skip ':'
            SKIP_WS(next);
            scan = next;
            if (*next == '-') {
                sign = -1;
                next++;
                SKIP_WS(next);
                scan = next;
            }
            KMP_ASSERT2((*next >=  '0') && (*next <= '9'),
              "bad explicit proc list");
            SKIP_DIGITS(next);
            stride = __kmp_str_to_int(scan, *next);
            KMP_ASSERT2(stride >= 0, "bad explicit proc list");
            stride *= sign;
        }

        //
        // Do some range checks.
        //
        KMP_ASSERT2(stride != 0, "bad explicit proc list");
        if (stride > 0) {
            KMP_ASSERT2(start <= end, "bad explicit proc list");
        }
        else {
            KMP_ASSERT2(start >= end, "bad explicit proc list");
        }
        KMP_ASSERT2((end - start) / stride <= 65536, "bad explicit proc list");

        //
        // Add the mask for each OS proc # to the list.
        //
        if (stride > 0) {
            do {
                ADD_MASK_OSID(start, osId2Mask, maxOsId);
                start += stride;
            } while (start <= end);
        }
        else {
            do {
                ADD_MASK_OSID(start, osId2Mask, maxOsId);
                start += stride;
            } while (start >= end);
        }

        //
        // Skip optional comma.
        //
        SKIP_WS(next);
        if (*next == ',') {
            next++;
        }
        scan = next;
    }

    *out_numMasks = nextNewMask;
    if (nextNewMask == 0) {
        *out_masks = NULL;
        KMP_INTERNAL_FREE(newMasks);
        return;
    }
    *out_masks
      = (kmp_affin_mask_t *)__kmp_allocate(nextNewMask * __kmp_affin_mask_size);
    KMP_MEMCPY(*out_masks, newMasks, nextNewMask * __kmp_affin_mask_size);
    __kmp_free(sumMask);
    KMP_INTERNAL_FREE(newMasks);
}


# if OMP_40_ENABLED

/*-----------------------------------------------------------------------------

Re-parse the OMP_PLACES proc id list, forming the newMasks for the different
places.  Again, Here is the grammar:

place_list := place
place_list := place , place_list
place := num
place := place : num
place := place : num : signed
place := { subplacelist }
place := ! place                  // (lowest priority)
subplace_list := subplace
subplace_list := subplace , subplace_list
subplace := num
subplace := num : num
subplace := num : num : signed
signed := num
signed := + signed
signed := - signed

-----------------------------------------------------------------------------*/

static void
__kmp_process_subplace_list(const char **scan, kmp_affin_mask_t *osId2Mask,
  int maxOsId, kmp_affin_mask_t *tempMask, int *setSize)
{
    const char *next;

    for (;;) {
        int start, count, stride, i;

        //
        // Read in the starting proc id
        //
        SKIP_WS(*scan);
        KMP_ASSERT2((**scan >= '0') && (**scan <= '9'),
          "bad explicit places list");
        next = *scan;
        SKIP_DIGITS(next);
        start = __kmp_str_to_int(*scan, *next);
        KMP_ASSERT(start >= 0);
        *scan = next;

        //
        // valid follow sets are ',' ':' and '}'
        //
        SKIP_WS(*scan);
        if (**scan == '}' || **scan == ',') {
            if ((start > maxOsId) ||
              (! KMP_CPU_ISSET(start, KMP_CPU_INDEX(osId2Mask, start)))) {
                if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                  && (__kmp_affinity_type != affinity_none))) {
                    KMP_WARNING(AffIgnoreInvalidProcID, start);
                }
            }
            else {
                KMP_CPU_UNION(tempMask, KMP_CPU_INDEX(osId2Mask, start));
                (*setSize)++;
            }
            if (**scan == '}') {
                break;
            }
            (*scan)++;  // skip ','
            continue;
        }
        KMP_ASSERT2(**scan == ':', "bad explicit places list");
        (*scan)++;      // skip ':'

        //
        // Read count parameter
        //
        SKIP_WS(*scan);
        KMP_ASSERT2((**scan >= '0') && (**scan <= '9'),
          "bad explicit places list");
        next = *scan;
        SKIP_DIGITS(next);
        count = __kmp_str_to_int(*scan, *next);
        KMP_ASSERT(count >= 0);
        *scan = next;

        //
        // valid follow sets are ',' ':' and '}'
        //
        SKIP_WS(*scan);
        if (**scan == '}' || **scan == ',') {
            for (i = 0; i < count; i++) {
                if ((start > maxOsId) ||
                  (! KMP_CPU_ISSET(start, KMP_CPU_INDEX(osId2Mask, start)))) {
                    if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                      && (__kmp_affinity_type != affinity_none))) {
                        KMP_WARNING(AffIgnoreInvalidProcID, start);
                    }
                    break;  // don't proliferate warnings for large count
                }
                else {
                    KMP_CPU_UNION(tempMask, KMP_CPU_INDEX(osId2Mask, start));
                    start++;
                    (*setSize)++;
                }
            }
            if (**scan == '}') {
                break;
            }
            (*scan)++;  // skip ','
            continue;
        }
        KMP_ASSERT2(**scan == ':', "bad explicit places list");
        (*scan)++;      // skip ':'

        //
        // Read stride parameter
        //
        int sign = +1;
        for (;;) {
            SKIP_WS(*scan);
            if (**scan == '+') {
                (*scan)++; // skip '+'
                continue;
            }
            if (**scan == '-') {
                sign *= -1;
                (*scan)++; // skip '-'
                continue;
            }
            break;
        }
        SKIP_WS(*scan);
        KMP_ASSERT2((**scan >= '0') && (**scan <= '9'),
          "bad explicit places list");
        next = *scan;
        SKIP_DIGITS(next);
        stride = __kmp_str_to_int(*scan, *next);
        KMP_ASSERT(stride >= 0);
        *scan = next;
        stride *= sign;

        //
        // valid follow sets are ',' and '}'
        //
        SKIP_WS(*scan);
        if (**scan == '}' || **scan == ',') {
            for (i = 0; i < count; i++) {
                if ((start > maxOsId) ||
                  (! KMP_CPU_ISSET(start, KMP_CPU_INDEX(osId2Mask, start)))) {
                    if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                      && (__kmp_affinity_type != affinity_none))) {
                        KMP_WARNING(AffIgnoreInvalidProcID, start);
                    }
                    break;  // don't proliferate warnings for large count
                }
                else {
                    KMP_CPU_UNION(tempMask, KMP_CPU_INDEX(osId2Mask, start));
                    start += stride;
                    (*setSize)++;
                }
            }
            if (**scan == '}') {
                break;
            }
            (*scan)++;  // skip ','
            continue;
        }

        KMP_ASSERT2(0, "bad explicit places list");
    }
}


static void
__kmp_process_place(const char **scan, kmp_affin_mask_t *osId2Mask,
  int maxOsId, kmp_affin_mask_t *tempMask, int *setSize)
{
    const char *next;

    //
    // valid follow sets are '{' '!' and num
    //
    SKIP_WS(*scan);
    if (**scan == '{') {
        (*scan)++;      // skip '{'
        __kmp_process_subplace_list(scan, osId2Mask, maxOsId , tempMask,
          setSize);
        KMP_ASSERT2(**scan == '}', "bad explicit places list");
        (*scan)++;      // skip '}'
    }
    else if (**scan == '!') {
        __kmp_process_place(scan, osId2Mask, maxOsId, tempMask, setSize);
        KMP_CPU_COMPLEMENT(tempMask);
        (*scan)++;      // skip '!'
    }
    else if ((**scan >= '0') && (**scan <= '9')) {
        next = *scan;
        SKIP_DIGITS(next);
        int num = __kmp_str_to_int(*scan, *next);
        KMP_ASSERT(num >= 0);
        if ((num > maxOsId) ||
          (! KMP_CPU_ISSET(num, KMP_CPU_INDEX(osId2Mask, num)))) {
            if (__kmp_affinity_verbose || (__kmp_affinity_warnings
              && (__kmp_affinity_type != affinity_none))) {
                KMP_WARNING(AffIgnoreInvalidProcID, num);
            }
        }
        else {
            KMP_CPU_UNION(tempMask, KMP_CPU_INDEX(osId2Mask, num));
            (*setSize)++;
        }
        *scan = next;  // skip num
    }
    else {
        KMP_ASSERT2(0, "bad explicit places list");
    }
}


//static void
void
__kmp_affinity_process_placelist(kmp_affin_mask_t **out_masks,
  unsigned int *out_numMasks, const char *placelist,
  kmp_affin_mask_t *osId2Mask, int maxOsId)
{
    const char *scan = placelist;
    const char *next = placelist;

    numNewMasks = 2;
    newMasks = (kmp_affin_mask_t *)KMP_INTERNAL_MALLOC(numNewMasks
      * __kmp_affin_mask_size);
    nextNewMask = 0;

    kmp_affin_mask_t *tempMask = (kmp_affin_mask_t *)__kmp_allocate(
      __kmp_affin_mask_size);
    KMP_CPU_ZERO(tempMask);
    int setSize = 0;

    for (;;) {
        __kmp_process_place(&scan, osId2Mask, maxOsId, tempMask, &setSize);

        //
        // valid follow sets are ',' ':' and EOL
        //
        SKIP_WS(scan);
        if (*scan == '\0' || *scan == ',') {
            if (setSize > 0) {
                ADD_MASK(tempMask);
            }
            KMP_CPU_ZERO(tempMask);
            setSize = 0;
            if (*scan == '\0') {
                break;
            }
            scan++;     // skip ','
            continue;
        }

        KMP_ASSERT2(*scan == ':', "bad explicit places list");
        scan++;         // skip ':'

        //
        // Read count parameter
        //
        SKIP_WS(scan);
        KMP_ASSERT2((*scan >= '0') && (*scan <= '9'),
          "bad explicit places list");
        next = scan;
        SKIP_DIGITS(next);
        int count = __kmp_str_to_int(scan, *next);
        KMP_ASSERT(count >= 0);
        scan = next;

        //
        // valid follow sets are ',' ':' and EOL
        //
        SKIP_WS(scan);
        int stride;
        if (*scan == '\0' || *scan == ',') {
            stride = +1;
        }
        else {
            KMP_ASSERT2(*scan == ':', "bad explicit places list");
            scan++;         // skip ':'

            //
            // Read stride parameter
            //
            int sign = +1;
            for (;;) {
                SKIP_WS(scan);
                if (*scan == '+') {
                    scan++; // skip '+'
                    continue;
                }
                if (*scan == '-') {
                    sign *= -1;
                    scan++; // skip '-'
                    continue;
                }
                break;
            }
            SKIP_WS(scan);
            KMP_ASSERT2((*scan >= '0') && (*scan <= '9'),
              "bad explicit places list");
            next = scan;
            SKIP_DIGITS(next);
            stride = __kmp_str_to_int(scan, *next);
            KMP_DEBUG_ASSERT(stride >= 0);
            scan = next;
            stride *= sign;
        }

        if (stride > 0) {
            int i;
            for (i = 0; i < count; i++) {
                int j;
                if (setSize == 0) {
                    break;
                }
                ADD_MASK(tempMask);
                setSize = 0;
                for (j = __kmp_affin_mask_size * CHAR_BIT - 1; j >= stride; j--) {
                    if (! KMP_CPU_ISSET(j - stride, tempMask)) {
                        KMP_CPU_CLR(j, tempMask);
                    }
                    else if ((j > maxOsId) ||
                      (! KMP_CPU_ISSET(j, KMP_CPU_INDEX(osId2Mask, j)))) {
                        if ((__kmp_affinity_verbose || (__kmp_affinity_warnings
                          && (__kmp_affinity_type != affinity_none))) && i < count - 1) {
                            KMP_WARNING(AffIgnoreInvalidProcID, j);
                        }
                        KMP_CPU_CLR(j, tempMask);
                    }
                    else {
                        KMP_CPU_SET(j, tempMask);
                        setSize++;
                    }
                }
                for (; j >= 0; j--) {
                    KMP_CPU_CLR(j, tempMask);
                }
            }
        }
        else {
            int i;
            for (i = 0; i < count; i++) {
                int j;
                if (setSize == 0) {
                    break;
                }
                ADD_MASK(tempMask);
                setSize = 0;
                for (j = 0; j < ((int)__kmp_affin_mask_size * CHAR_BIT) + stride;
                  j++) {
                    if (! KMP_CPU_ISSET(j - stride, tempMask)) {
                        KMP_CPU_CLR(j, tempMask);
                    }
                    else if ((j > maxOsId) ||
                      (! KMP_CPU_ISSET(j, KMP_CPU_INDEX(osId2Mask, j)))) {
                        if ((__kmp_affinity_verbose || (__kmp_affinity_warnings
                          && (__kmp_affinity_type != affinity_none))) && i < count - 1) {
                            KMP_WARNING(AffIgnoreInvalidProcID, j);
                        }
                        KMP_CPU_CLR(j, tempMask);
                    }
                    else {
                        KMP_CPU_SET(j, tempMask);
                        setSize++;
                    }
                }
                for (; j < (int)__kmp_affin_mask_size * CHAR_BIT; j++) {
                    KMP_CPU_CLR(j, tempMask);
                }
            }
        }
        KMP_CPU_ZERO(tempMask);
        setSize = 0;

        //
        // valid follow sets are ',' and EOL
        //
        SKIP_WS(scan);
        if (*scan == '\0') {
            break;
        }
        if (*scan == ',') {
            scan++;     // skip ','
            continue;
        }

        KMP_ASSERT2(0, "bad explicit places list");
    }

    *out_numMasks = nextNewMask;
    if (nextNewMask == 0) {
        *out_masks = NULL;
        KMP_INTERNAL_FREE(newMasks);
        return;
    }
    *out_masks
      = (kmp_affin_mask_t *)__kmp_allocate(nextNewMask * __kmp_affin_mask_size);
    KMP_MEMCPY(*out_masks, newMasks, nextNewMask * __kmp_affin_mask_size);
    __kmp_free(tempMask);
    KMP_INTERNAL_FREE(newMasks);
}

# endif /* OMP_40_ENABLED */

#undef ADD_MASK
#undef ADD_MASK_OSID

static void
__kmp_apply_thread_places(AddrUnsPair **pAddr, int depth)
{
    if ( __kmp_place_num_cores == 0 ) {
        if ( __kmp_place_num_threads_per_core == 0 ) {
            return;   // no cores limiting actions requested, exit
        }
        __kmp_place_num_cores = nCoresPerPkg;   // use all available cores
    }
    if ( !__kmp_affinity_uniform_topology() ) {
        KMP_WARNING( AffThrPlaceNonUniform );
        return; // don't support non-uniform topology
    }
    if ( depth != 3 ) {
        KMP_WARNING( AffThrPlaceNonThreeLevel );
        return; // don't support not-3-level topology
    }
    if ( __kmp_place_num_threads_per_core == 0 ) {
        __kmp_place_num_threads_per_core = __kmp_nThreadsPerCore;  // use all HW contexts
    }
    if ( __kmp_place_core_offset + __kmp_place_num_cores > nCoresPerPkg ) {
        KMP_WARNING( AffThrPlaceManyCores );
        return;
    }

    AddrUnsPair *newAddr = (AddrUnsPair *)__kmp_allocate( sizeof(AddrUnsPair) *
                            nPackages * __kmp_place_num_cores * __kmp_place_num_threads_per_core);
    int i, j, k, n_old = 0, n_new = 0;
    for ( i = 0; i < nPackages; ++i ) {
        for ( j = 0; j < nCoresPerPkg; ++j ) {
            if ( j < __kmp_place_core_offset || j >= __kmp_place_core_offset + __kmp_place_num_cores ) {
                n_old += __kmp_nThreadsPerCore;   // skip not-requested core
            } else {
                for ( k = 0; k < __kmp_nThreadsPerCore; ++k ) {
                    if ( k < __kmp_place_num_threads_per_core ) {
                        newAddr[n_new] = (*pAddr)[n_old];   // copy requested core' data to new location
                        n_new++;
                    }
                    n_old++;
                }
            }
        }
    }
    nCoresPerPkg = __kmp_place_num_cores;                     // correct nCoresPerPkg
    __kmp_nThreadsPerCore = __kmp_place_num_threads_per_core; // correct __kmp_nThreadsPerCore
    __kmp_avail_proc = n_new;                                 // correct avail_proc
    __kmp_ncores = nPackages * __kmp_place_num_cores;         // correct ncores

    __kmp_free( *pAddr );
    *pAddr = newAddr;      // replace old topology with new one
}


static AddrUnsPair *address2os = NULL;
static int           * procarr = NULL;
static int     __kmp_aff_depth = 0;

static void
__kmp_aux_affinity_initialize(void)
{
    if (__kmp_affinity_masks != NULL) {
        KMP_ASSERT(fullMask != NULL);
        return;
    }

    //
    // Create the "full" mask - this defines all of the processors that we
    // consider to be in the machine model.  If respect is set, then it is
    // the initialization thread's affinity mask.  Otherwise, it is all
    // processors that we know about on the machine.
    //
    if (fullMask == NULL) {
        fullMask = (kmp_affin_mask_t *)__kmp_allocate(__kmp_affin_mask_size);
    }
    if (KMP_AFFINITY_CAPABLE()) {
        if (__kmp_affinity_respect_mask) {
            __kmp_get_system_affinity(fullMask, TRUE);

            //
            // Count the number of available processors.
            //
            unsigned i;

#if KMP_OS_CNK
            for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
                // Under CNK, threads don't really have affinity masks, but
                // rather, are assigned to a single thread. As a result, when
                // asked for the initial thread's affinity mask, you get back a
                // mask with only one bit set (the first bit of those available
                // to the process).
                if (! KMP_CPU_ISSET((i/__kmp_xproc) * __kmp_xproc, fullMask)) {
                    continue;
                }

                KMP_CPU_SET(i, fullMask);
            }
#endif

            __kmp_avail_proc = 0;
            for (i = 0; i < KMP_CPU_SETSIZE; ++i) {
                if (! KMP_CPU_ISSET(i, fullMask)) {
                    continue;
                }
                __kmp_avail_proc++;
            }
            if (__kmp_avail_proc > __kmp_xproc) {
                if (__kmp_affinity_verbose || (__kmp_affinity_warnings
                  && (__kmp_affinity_type != affinity_none))) {
                    KMP_WARNING(ErrorInitializeAffinity);
                }
                __kmp_affinity_type = affinity_none;
                KMP_AFFINITY_DISABLE();
                return;
            }
        }
        else {
            __kmp_affinity_entire_machine_mask(fullMask);
            __kmp_avail_proc = __kmp_xproc;
        }
    }

    int depth = -1;
    kmp_i18n_id_t msg_id = kmp_i18n_null;

    //
    // For backward compatibility, setting KMP_CPUINFO_FILE =>
    // KMP_TOPOLOGY_METHOD=cpuinfo
    //
    if ((__kmp_cpuinfo_file != NULL) &&
      (__kmp_affinity_top_method == affinity_top_method_all)) {
        __kmp_affinity_top_method = affinity_top_method_cpuinfo;
    }

    if (__kmp_affinity_top_method == affinity_top_method_all) {
        //
        // In the default code path, errors are not fatal - we just try using
        // another method.  We only emit a warning message if affinity is on,
        // or the verbose flag is set, an the nowarnings flag was not set.
        //
        const char *file_name = NULL;
        int line = 0;

# if KMP_ARCH_X86 || KMP_ARCH_X86_64

        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffInfoStr, "KMP_AFFINITY", KMP_I18N_STR(Decodingx2APIC));
        }

        file_name = NULL;
        depth = __kmp_affinity_create_x2apicid_map(&address2os, &msg_id);
        if (depth == 0) {
            KMP_ASSERT(__kmp_affinity_type == affinity_none);
            KMP_ASSERT(address2os == NULL);
            return;
        }

        if (depth < 0) {
            if (__kmp_affinity_verbose) {
                if (msg_id != kmp_i18n_null) {
                    KMP_INFORM(AffInfoStrStr, "KMP_AFFINITY", __kmp_i18n_catgets(msg_id),
                      KMP_I18N_STR(DecodingLegacyAPIC));
                }
                else {
                    KMP_INFORM(AffInfoStr, "KMP_AFFINITY", KMP_I18N_STR(DecodingLegacyAPIC));
                }
            }

            file_name = NULL;
            depth = __kmp_affinity_create_apicid_map(&address2os, &msg_id);
            if (depth == 0) {
                KMP_ASSERT(__kmp_affinity_type == affinity_none);
                KMP_ASSERT(address2os == NULL);
                return;
            }
        }

# endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

# if (KMP_OS_LINUX && !KMP_OS_CNK)

        if (depth < 0) {
            if (__kmp_affinity_verbose) {
                if (msg_id != kmp_i18n_null) {
                    KMP_INFORM(AffStrParseFilename, "KMP_AFFINITY", __kmp_i18n_catgets(msg_id), "/proc/cpuinfo");
                }
                else {
                    KMP_INFORM(AffParseFilename, "KMP_AFFINITY", "/proc/cpuinfo");
                }
            }

            FILE *f = fopen("/proc/cpuinfo", "r");
            if (f == NULL) {
                msg_id = kmp_i18n_str_CantOpenCpuinfo;
            }
            else {
                file_name = "/proc/cpuinfo";
                depth = __kmp_affinity_create_cpuinfo_map(&address2os, &line, &msg_id, f);
                fclose(f);
                if (depth == 0) {
                    KMP_ASSERT(__kmp_affinity_type == affinity_none);
                    KMP_ASSERT(address2os == NULL);
                    return;
                }
            }
        }

# endif /* KMP_OS_LINUX */

# if KMP_GROUP_AFFINITY

        if ((depth < 0) && (__kmp_num_proc_groups > 1)) {
            if (__kmp_affinity_verbose) {
                KMP_INFORM(AffWindowsProcGroupMap, "KMP_AFFINITY");
            }

            depth = __kmp_affinity_create_proc_group_map(&address2os, &msg_id);
            KMP_ASSERT(depth != 0);
        }

# endif /* KMP_GROUP_AFFINITY */

#if KMP_OS_CNK
        if (depth < 0) {
            depth = __kmp_affinity_create_bgq_map(&address2os, &msg_id);
            if (depth > 0) {
              KMP_ASSERT(address2os != NULL);
            }
        }
#endif

        if (depth < 0) {
            if (__kmp_affinity_verbose && (msg_id != kmp_i18n_null)) {
                if (file_name == NULL) {
                    KMP_INFORM(UsingFlatOS, __kmp_i18n_catgets(msg_id));
                }
                else if (line == 0) {
                    KMP_INFORM(UsingFlatOSFile, file_name, __kmp_i18n_catgets(msg_id));
                }
                else {
                    KMP_INFORM(UsingFlatOSFileLine, file_name, line, __kmp_i18n_catgets(msg_id));
                }
            }
            // FIXME - print msg if msg_id = kmp_i18n_null ???

            file_name = "";
            depth = __kmp_affinity_create_flat_map(&address2os, &msg_id);
            if (depth == 0) {
                KMP_ASSERT(__kmp_affinity_type == affinity_none);
                KMP_ASSERT(address2os == NULL);
                return;
            }
            KMP_ASSERT(depth > 0);
            KMP_ASSERT(address2os != NULL);
        }
    }

    //
    // If the user has specified that a paricular topology discovery method
    // is to be used, then we abort if that method fails.  The exception is
    // group affinity, which might have been implicitly set.
    //

# if KMP_ARCH_X86 || KMP_ARCH_X86_64

    else if (__kmp_affinity_top_method == affinity_top_method_x2apicid) {
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffInfoStr, "KMP_AFFINITY",
              KMP_I18N_STR(Decodingx2APIC));
        }

        depth = __kmp_affinity_create_x2apicid_map(&address2os, &msg_id);
        if (depth == 0) {
            KMP_ASSERT(__kmp_affinity_type == affinity_none);
            KMP_ASSERT(address2os == NULL);
            return;
        }
        if (depth < 0) {
            KMP_ASSERT(msg_id != kmp_i18n_null);
            KMP_FATAL(MsgExiting, __kmp_i18n_catgets(msg_id));
        }
    }
    else if (__kmp_affinity_top_method == affinity_top_method_apicid) {
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffInfoStr, "KMP_AFFINITY",
              KMP_I18N_STR(DecodingLegacyAPIC));
        }

        depth = __kmp_affinity_create_apicid_map(&address2os, &msg_id);
        if (depth == 0) {
            KMP_ASSERT(__kmp_affinity_type == affinity_none);
            KMP_ASSERT(address2os == NULL);
            return;
        }
        if (depth < 0) {
            KMP_ASSERT(msg_id != kmp_i18n_null);
            KMP_FATAL(MsgExiting, __kmp_i18n_catgets(msg_id));
        }
    }

# endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

    else if (__kmp_affinity_top_method == affinity_top_method_cpuinfo) {
        const char *filename;
        if (__kmp_cpuinfo_file != NULL) {
            filename = __kmp_cpuinfo_file;
        }
        else {
            filename = "/proc/cpuinfo";
        }

        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffParseFilename, "KMP_AFFINITY", filename);
        }

        FILE *f = fopen(filename, "r");
        if (f == NULL) {
            int code = errno;
            if (__kmp_cpuinfo_file != NULL) {
                __kmp_msg(
                    kmp_ms_fatal,
                    KMP_MSG(CantOpenFileForReading, filename),
                    KMP_ERR(code),
                    KMP_HNT(NameComesFrom_CPUINFO_FILE),
                    __kmp_msg_null
                );
            }
            else {
                __kmp_msg(
                    kmp_ms_fatal,
                    KMP_MSG(CantOpenFileForReading, filename),
                    KMP_ERR(code),
                    __kmp_msg_null
                );
            }
        }
        int line = 0;
        depth = __kmp_affinity_create_cpuinfo_map(&address2os, &line, &msg_id, f);
        fclose(f);
        if (depth < 0) {
            KMP_ASSERT(msg_id != kmp_i18n_null);
            if (line > 0) {
                KMP_FATAL(FileLineMsgExiting, filename, line, __kmp_i18n_catgets(msg_id));
            }
            else {
                KMP_FATAL(FileMsgExiting, filename, __kmp_i18n_catgets(msg_id));
            }
        }
        if (__kmp_affinity_type == affinity_none) {
            KMP_ASSERT(depth == 0);
            KMP_ASSERT(address2os == NULL);
            return;
        }
    }

# if KMP_GROUP_AFFINITY

    else if (__kmp_affinity_top_method == affinity_top_method_group) {
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffWindowsProcGroupMap, "KMP_AFFINITY");
        }

        depth = __kmp_affinity_create_proc_group_map(&address2os, &msg_id);
        KMP_ASSERT(depth != 0);
        if (depth < 0) {
            KMP_ASSERT(msg_id != kmp_i18n_null);
            KMP_FATAL(MsgExiting, __kmp_i18n_catgets(msg_id));
        }
    }

# endif /* KMP_GROUP_AFFINITY */

    else if (__kmp_affinity_top_method == affinity_top_method_flat) {
        if (__kmp_affinity_verbose) {
            KMP_INFORM(AffUsingFlatOS, "KMP_AFFINITY");
        }

        depth = __kmp_affinity_create_flat_map(&address2os, &msg_id);
        if (depth == 0) {
            KMP_ASSERT(__kmp_affinity_type == affinity_none);
            KMP_ASSERT(address2os == NULL);
            return;
        }
        // should not fail
        KMP_ASSERT(depth > 0);
        KMP_ASSERT(address2os != NULL);
    }

    if (address2os == NULL) {
        if (KMP_AFFINITY_CAPABLE()
          && (__kmp_affinity_verbose || (__kmp_affinity_warnings
          && (__kmp_affinity_type != affinity_none)))) {
            KMP_WARNING(ErrorInitializeAffinity);
        }
        __kmp_affinity_type = affinity_none;
        KMP_AFFINITY_DISABLE();
        return;
    }

    __kmp_apply_thread_places(&address2os, depth);

    //
    // Create the table of masks, indexed by thread Id.
    //
    unsigned maxIndex;
    unsigned numUnique;
    kmp_affin_mask_t *osId2Mask = __kmp_create_masks(&maxIndex, &numUnique,
      address2os, __kmp_avail_proc);
    if (__kmp_affinity_gran_levels == 0) {
        KMP_DEBUG_ASSERT((int)numUnique == __kmp_avail_proc);
    }

    //
    // Set the childNums vector in all Address objects.  This must be done
    // before we can sort using __kmp_affinity_cmp_Address_child_num(),
    // which takes into account the setting of __kmp_affinity_compact.
    //
    __kmp_affinity_assign_child_nums(address2os, __kmp_avail_proc);

    switch (__kmp_affinity_type) {

        case affinity_explicit:
        KMP_DEBUG_ASSERT(__kmp_affinity_proclist != NULL);
# if OMP_40_ENABLED
        if (__kmp_nested_proc_bind.bind_types[0] == proc_bind_intel)
# endif
        {
            __kmp_affinity_process_proclist(&__kmp_affinity_masks,
              &__kmp_affinity_num_masks, __kmp_affinity_proclist, osId2Mask,
              maxIndex);
        }
# if OMP_40_ENABLED
        else {
            __kmp_affinity_process_placelist(&__kmp_affinity_masks,
              &__kmp_affinity_num_masks, __kmp_affinity_proclist, osId2Mask,
              maxIndex);
        }
# endif
        if (__kmp_affinity_num_masks == 0) {
            if (__kmp_affinity_verbose || (__kmp_affinity_warnings
              && (__kmp_affinity_type != affinity_none))) {
                KMP_WARNING(AffNoValidProcID);
            }
            __kmp_affinity_type = affinity_none;
            return;
        }
        break;

        //
        // The other affinity types rely on sorting the Addresses according
        // to some permutation of the machine topology tree.  Set
        // __kmp_affinity_compact and __kmp_affinity_offset appropriately,
        // then jump to a common code fragment to do the sort and create
        // the array of affinity masks.
        //

        case affinity_logical:
        __kmp_affinity_compact = 0;
        if (__kmp_affinity_offset) {
            __kmp_affinity_offset = __kmp_nThreadsPerCore * __kmp_affinity_offset
              % __kmp_avail_proc;
        }
        goto sortAddresses;

        case affinity_physical:
        if (__kmp_nThreadsPerCore > 1) {
            __kmp_affinity_compact = 1;
            if (__kmp_affinity_compact >= depth) {
                __kmp_affinity_compact = 0;
            }
        } else {
            __kmp_affinity_compact = 0;
        }
        if (__kmp_affinity_offset) {
            __kmp_affinity_offset = __kmp_nThreadsPerCore * __kmp_affinity_offset
              % __kmp_avail_proc;
        }
        goto sortAddresses;

        case affinity_scatter:
        if (__kmp_affinity_compact >= depth) {
            __kmp_affinity_compact = 0;
        }
        else {
            __kmp_affinity_compact = depth - 1 - __kmp_affinity_compact;
        }
        goto sortAddresses;

        case affinity_compact:
        if (__kmp_affinity_compact >= depth) {
            __kmp_affinity_compact = depth - 1;
        }
        goto sortAddresses;

        case affinity_balanced:
        // Balanced works only for the case of a single package
        if( nPackages > 1 ) {
            if( __kmp_affinity_verbose || __kmp_affinity_warnings ) {
                KMP_WARNING( AffBalancedNotAvail, "KMP_AFFINITY" );
            }
            __kmp_affinity_type = affinity_none;
            return;
        } else if( __kmp_affinity_uniform_topology() ) {
            break;
        } else { // Non-uniform topology

            // Save the depth for further usage
            __kmp_aff_depth = depth;

            // Number of hyper threads per core in HT machine
            int nth_per_core = __kmp_nThreadsPerCore;

            int core_level;
            if( nth_per_core > 1 ) {
                core_level = depth - 2;
            } else {
                core_level = depth - 1;
            }
            int ncores = address2os[ __kmp_avail_proc - 1 ].first.labels[ core_level ] + 1;
            int nproc = nth_per_core * ncores;

            procarr = ( int * )__kmp_allocate( sizeof( int ) * nproc );
            for( int i = 0; i < nproc; i++ ) {
                procarr[ i ] = -1;
            }

            for( int i = 0; i < __kmp_avail_proc; i++ ) {
                int proc = address2os[ i ].second;
                // If depth == 3 then level=0 - package, level=1 - core, level=2 - thread.
                // If there is only one thread per core then depth == 2: level 0 - package,
                // level 1 - core.
                int level = depth - 1;

                // __kmp_nth_per_core == 1
                int thread = 0;
                int core = address2os[ i ].first.labels[ level ];
                // If the thread level exists, that is we have more than one thread context per core
                if( nth_per_core > 1 ) {
                    thread = address2os[ i ].first.labels[ level ] % nth_per_core;
                    core = address2os[ i ].first.labels[ level - 1 ];
                }
                procarr[ core * nth_per_core + thread ] = proc;
            }

            break;
        }

        sortAddresses:
        //
        // Allocate the gtid->affinity mask table.
        //
        if (__kmp_affinity_dups) {
            __kmp_affinity_num_masks = __kmp_avail_proc;
        }
        else {
            __kmp_affinity_num_masks = numUnique;
        }

# if OMP_40_ENABLED
        if ( ( __kmp_nested_proc_bind.bind_types[0] != proc_bind_intel )
          && ( __kmp_affinity_num_places > 0 )
          && ( (unsigned)__kmp_affinity_num_places < __kmp_affinity_num_masks ) ) {
            __kmp_affinity_num_masks = __kmp_affinity_num_places;
        }
# endif

        __kmp_affinity_masks = (kmp_affin_mask_t*)__kmp_allocate(
          __kmp_affinity_num_masks * __kmp_affin_mask_size);

        //
        // Sort the address2os table according to the current setting of
        // __kmp_affinity_compact, then fill out __kmp_affinity_masks.
        //
        qsort(address2os, __kmp_avail_proc, sizeof(*address2os),
          __kmp_affinity_cmp_Address_child_num);
        {
            int i;
            unsigned j;
            for (i = 0, j = 0; i < __kmp_avail_proc; i++) {
                if ((! __kmp_affinity_dups) && (! address2os[i].first.leader)) {
                    continue;
                }
                unsigned osId = address2os[i].second;
                kmp_affin_mask_t *src = KMP_CPU_INDEX(osId2Mask, osId);
                kmp_affin_mask_t *dest
                  = KMP_CPU_INDEX(__kmp_affinity_masks, j);
                KMP_ASSERT(KMP_CPU_ISSET(osId, src));
                KMP_CPU_COPY(dest, src);
                if (++j >= __kmp_affinity_num_masks) {
                    break;
                }
            }
            KMP_DEBUG_ASSERT(j == __kmp_affinity_num_masks);
        }
        break;

        default:
        KMP_ASSERT2(0, "Unexpected affinity setting");
    }

    __kmp_free(osId2Mask);
    machine_hierarchy.init(address2os, __kmp_avail_proc);
}


void
__kmp_affinity_initialize(void)
{
    //
    // Much of the code above was written assumming that if a machine was not
    // affinity capable, then __kmp_affinity_type == affinity_none.  We now
    // explicitly represent this as __kmp_affinity_type == affinity_disabled.
    //
    // There are too many checks for __kmp_affinity_type == affinity_none
    // in this code.  Instead of trying to change them all, check if
    // __kmp_affinity_type == affinity_disabled, and if so, slam it with
    // affinity_none, call the real initialization routine, then restore
    // __kmp_affinity_type to affinity_disabled.
    //
    int disabled = (__kmp_affinity_type == affinity_disabled);
    if (! KMP_AFFINITY_CAPABLE()) {
        KMP_ASSERT(disabled);
    }
    if (disabled) {
        __kmp_affinity_type = affinity_none;
    }
    __kmp_aux_affinity_initialize();
    if (disabled) {
        __kmp_affinity_type = affinity_disabled;
    }
}


void
__kmp_affinity_uninitialize(void)
{
    if (__kmp_affinity_masks != NULL) {
        __kmp_free(__kmp_affinity_masks);
        __kmp_affinity_masks = NULL;
    }
    if (fullMask != NULL) {
        KMP_CPU_FREE(fullMask);
        fullMask = NULL;
    }
    __kmp_affinity_num_masks = 0;
# if OMP_40_ENABLED
    __kmp_affinity_num_places = 0;
# endif
    if (__kmp_affinity_proclist != NULL) {
        __kmp_free(__kmp_affinity_proclist);
        __kmp_affinity_proclist = NULL;
    }
    if( address2os != NULL ) {
        __kmp_free( address2os );
        address2os = NULL;
    }
    if( procarr != NULL ) {
        __kmp_free( procarr );
        procarr = NULL;
    }
}


void
__kmp_affinity_set_init_mask(int gtid, int isa_root)
{
    if (! KMP_AFFINITY_CAPABLE()) {
        return;
    }

    kmp_info_t *th = (kmp_info_t *)TCR_SYNC_PTR(__kmp_threads[gtid]);
    if (th->th.th_affin_mask == NULL) {
        KMP_CPU_ALLOC(th->th.th_affin_mask);
    }
    else {
        KMP_CPU_ZERO(th->th.th_affin_mask);
    }

    //
    // Copy the thread mask to the kmp_info_t strucuture.
    // If __kmp_affinity_type == affinity_none, copy the "full" mask, i.e. one
    // that has all of the OS proc ids set, or if __kmp_affinity_respect_mask
    // is set, then the full mask is the same as the mask of the initialization
    // thread.
    //
    kmp_affin_mask_t *mask;
    int i;

# if OMP_40_ENABLED
    if (__kmp_nested_proc_bind.bind_types[0] == proc_bind_intel)
# endif
    {
        if ((__kmp_affinity_type == affinity_none) || (__kmp_affinity_type == affinity_balanced)
          ) {
# if KMP_GROUP_AFFINITY
            if (__kmp_num_proc_groups > 1) {
                return;
            }
# endif
            KMP_ASSERT(fullMask != NULL);
            i = KMP_PLACE_ALL;
            mask = fullMask;
        }
        else {
            KMP_DEBUG_ASSERT( __kmp_affinity_num_masks > 0 );
            i = (gtid + __kmp_affinity_offset) % __kmp_affinity_num_masks;
            mask = KMP_CPU_INDEX(__kmp_affinity_masks, i);
        }
    }
# if OMP_40_ENABLED
    else {
        if ((! isa_root)
          || (__kmp_nested_proc_bind.bind_types[0] == proc_bind_false)) {
#  if KMP_GROUP_AFFINITY
            if (__kmp_num_proc_groups > 1) {
                return;
            }
#  endif
            KMP_ASSERT(fullMask != NULL);
            i = KMP_PLACE_ALL;
            mask = fullMask;
        }
        else {
            //
            // int i = some hash function or just a counter that doesn't
            // always start at 0.  Use gtid for now.
            //
            KMP_DEBUG_ASSERT( __kmp_affinity_num_masks > 0 );
            i = (gtid + __kmp_affinity_offset) % __kmp_affinity_num_masks;
            mask = KMP_CPU_INDEX(__kmp_affinity_masks, i);
        }
    }
# endif

# if OMP_40_ENABLED
    th->th.th_current_place = i;
    if (isa_root) {
        th->th.th_new_place = i;
        th->th.th_first_place = 0;
        th->th.th_last_place = __kmp_affinity_num_masks - 1;
    }

    if (i == KMP_PLACE_ALL) {
        KA_TRACE(100, ("__kmp_affinity_set_init_mask: binding T#%d to all places\n",
          gtid));
    }
    else {
        KA_TRACE(100, ("__kmp_affinity_set_init_mask: binding T#%d to place %d\n",
          gtid, i));
    }
# else
    if (i == -1) {
        KA_TRACE(100, ("__kmp_affinity_set_init_mask: binding T#%d to fullMask\n",
          gtid));
    }
    else {
        KA_TRACE(100, ("__kmp_affinity_set_init_mask: binding T#%d to mask %d\n",
          gtid, i));
    }
# endif /* OMP_40_ENABLED */

    KMP_CPU_COPY(th->th.th_affin_mask, mask);

    if (__kmp_affinity_verbose) {
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          th->th.th_affin_mask);
        KMP_INFORM(BoundToOSProcSet, "KMP_AFFINITY", (kmp_int32)getpid(), gtid,
          buf);
    }

# if KMP_OS_WINDOWS
    //
    // On Windows* OS, the process affinity mask might have changed.
    // If the user didn't request affinity and this call fails,
    // just continue silently.  See CQ171393.
    //
    if ( __kmp_affinity_type == affinity_none ) {
        __kmp_set_system_affinity(th->th.th_affin_mask, FALSE);
    }
    else
# endif
    __kmp_set_system_affinity(th->th.th_affin_mask, TRUE);
}


# if OMP_40_ENABLED

void
__kmp_affinity_set_place(int gtid)
{
    int retval;

    if (! KMP_AFFINITY_CAPABLE()) {
        return;
    }

    kmp_info_t *th = (kmp_info_t *)TCR_SYNC_PTR(__kmp_threads[gtid]);

    KA_TRACE(100, ("__kmp_affinity_set_place: binding T#%d to place %d (current place = %d)\n",
      gtid, th->th.th_new_place, th->th.th_current_place));

    //
    // Check that the new place is within this thread's partition.
    //
    KMP_DEBUG_ASSERT(th->th.th_affin_mask != NULL);
    KMP_ASSERT(th->th.th_new_place >= 0);
    KMP_ASSERT((unsigned)th->th.th_new_place <= __kmp_affinity_num_masks);
    if (th->th.th_first_place <= th->th.th_last_place) {
        KMP_ASSERT((th->th.th_new_place >= th->th.th_first_place)
         && (th->th.th_new_place <= th->th.th_last_place));
    }
    else {
        KMP_ASSERT((th->th.th_new_place <= th->th.th_first_place)
         || (th->th.th_new_place >= th->th.th_last_place));
    }

    //
    // Copy the thread mask to the kmp_info_t strucuture,
    // and set this thread's affinity.
    //
    kmp_affin_mask_t *mask = KMP_CPU_INDEX(__kmp_affinity_masks,
      th->th.th_new_place);
    KMP_CPU_COPY(th->th.th_affin_mask, mask);
    th->th.th_current_place = th->th.th_new_place;

    if (__kmp_affinity_verbose) {
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          th->th.th_affin_mask);
        KMP_INFORM(BoundToOSProcSet, "OMP_PROC_BIND", (kmp_int32)getpid(),
          gtid, buf);
    }
    __kmp_set_system_affinity(th->th.th_affin_mask, TRUE);
}

# endif /* OMP_40_ENABLED */


int
__kmp_aux_set_affinity(void **mask)
{
    int gtid;
    kmp_info_t *th;
    int retval;

    if (! KMP_AFFINITY_CAPABLE()) {
        return -1;
    }

    gtid = __kmp_entry_gtid();
    KA_TRACE(1000, ;{
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          (kmp_affin_mask_t *)(*mask));
        __kmp_debug_printf("kmp_set_affinity: setting affinity mask for thread %d = %s\n",
          gtid, buf);
    });

    if (__kmp_env_consistency_check) {
        if ((mask == NULL) || (*mask == NULL)) {
            KMP_FATAL(AffinityInvalidMask, "kmp_set_affinity");
        }
        else {
            unsigned proc;
            int num_procs = 0;

            for (proc = 0; proc < KMP_CPU_SETSIZE; proc++) {
                if (! KMP_CPU_ISSET(proc, (kmp_affin_mask_t *)(*mask))) {
                    continue;
                }
                num_procs++;
                if (! KMP_CPU_ISSET(proc, fullMask)) {
                    KMP_FATAL(AffinityInvalidMask, "kmp_set_affinity");
                    break;
                }
            }
            if (num_procs == 0) {
                KMP_FATAL(AffinityInvalidMask, "kmp_set_affinity");
            }

# if KMP_GROUP_AFFINITY
            if (__kmp_get_proc_group((kmp_affin_mask_t *)(*mask)) < 0) {
                KMP_FATAL(AffinityInvalidMask, "kmp_set_affinity");
            }
# endif /* KMP_GROUP_AFFINITY */

        }
    }

    th = __kmp_threads[gtid];
    KMP_DEBUG_ASSERT(th->th.th_affin_mask != NULL);
    retval = __kmp_set_system_affinity((kmp_affin_mask_t *)(*mask), FALSE);
    if (retval == 0) {
        KMP_CPU_COPY(th->th.th_affin_mask, (kmp_affin_mask_t *)(*mask));
    }

# if OMP_40_ENABLED
    th->th.th_current_place = KMP_PLACE_UNDEFINED;
    th->th.th_new_place = KMP_PLACE_UNDEFINED;
    th->th.th_first_place = 0;
    th->th.th_last_place = __kmp_affinity_num_masks - 1;

    //
    // Turn off 4.0 affinity for the current tread at this parallel level.
    //
    th->th.th_current_task->td_icvs.proc_bind = proc_bind_false;
# endif

    return retval;
}


int
__kmp_aux_get_affinity(void **mask)
{
    int gtid;
    int retval;
    kmp_info_t *th;

    if (! KMP_AFFINITY_CAPABLE()) {
        return -1;
    }

    gtid = __kmp_entry_gtid();
    th = __kmp_threads[gtid];
    KMP_DEBUG_ASSERT(th->th.th_affin_mask != NULL);

    KA_TRACE(1000, ;{
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          th->th.th_affin_mask);
        __kmp_printf("kmp_get_affinity: stored affinity mask for thread %d = %s\n", gtid, buf);
    });

    if (__kmp_env_consistency_check) {
        if ((mask == NULL) || (*mask == NULL)) {
            KMP_FATAL(AffinityInvalidMask, "kmp_get_affinity");
        }
    }

# if !KMP_OS_WINDOWS

    retval = __kmp_get_system_affinity((kmp_affin_mask_t *)(*mask), FALSE);
    KA_TRACE(1000, ;{
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          (kmp_affin_mask_t *)(*mask));
        __kmp_printf("kmp_get_affinity: system affinity mask for thread %d = %s\n", gtid, buf);
    });
    return retval;

# else

    KMP_CPU_COPY((kmp_affin_mask_t *)(*mask), th->th.th_affin_mask);
    return 0;

# endif /* KMP_OS_WINDOWS */

}

int
__kmp_aux_set_affinity_mask_proc(int proc, void **mask)
{
    int retval;

    if (! KMP_AFFINITY_CAPABLE()) {
        return -1;
    }

    KA_TRACE(1000, ;{
        int gtid = __kmp_entry_gtid();
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          (kmp_affin_mask_t *)(*mask));
        __kmp_debug_printf("kmp_set_affinity_mask_proc: setting proc %d in affinity mask for thread %d = %s\n",
          proc, gtid, buf);
    });

    if (__kmp_env_consistency_check) {
        if ((mask == NULL) || (*mask == NULL)) {
            KMP_FATAL(AffinityInvalidMask, "kmp_set_affinity_mask_proc");
        }
    }

    if ((proc < 0) || ((unsigned)proc >= KMP_CPU_SETSIZE)) {
        return -1;
    }
    if (! KMP_CPU_ISSET(proc, fullMask)) {
        return -2;
    }

    KMP_CPU_SET(proc, (kmp_affin_mask_t *)(*mask));
    return 0;
}


int
__kmp_aux_unset_affinity_mask_proc(int proc, void **mask)
{
    int retval;

    if (! KMP_AFFINITY_CAPABLE()) {
        return -1;
    }

    KA_TRACE(1000, ;{
        int gtid = __kmp_entry_gtid();
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          (kmp_affin_mask_t *)(*mask));
        __kmp_debug_printf("kmp_unset_affinity_mask_proc: unsetting proc %d in affinity mask for thread %d = %s\n",
          proc, gtid, buf);
    });

    if (__kmp_env_consistency_check) {
        if ((mask == NULL) || (*mask == NULL)) {
            KMP_FATAL(AffinityInvalidMask, "kmp_unset_affinity_mask_proc");
        }
    }

    if ((proc < 0) || ((unsigned)proc >= KMP_CPU_SETSIZE)) {
        return -1;
    }
    if (! KMP_CPU_ISSET(proc, fullMask)) {
        return -2;
    }

    KMP_CPU_CLR(proc, (kmp_affin_mask_t *)(*mask));
    return 0;
}


int
__kmp_aux_get_affinity_mask_proc(int proc, void **mask)
{
    int retval;

    if (! KMP_AFFINITY_CAPABLE()) {
        return -1;
    }

    KA_TRACE(1000, ;{
        int gtid = __kmp_entry_gtid();
        char buf[KMP_AFFIN_MASK_PRINT_LEN];
        __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
          (kmp_affin_mask_t *)(*mask));
        __kmp_debug_printf("kmp_get_affinity_mask_proc: getting proc %d in affinity mask for thread %d = %s\n",
          proc, gtid, buf);
    });

    if (__kmp_env_consistency_check) {
        if ((mask == NULL) || (*mask == NULL)) {
            KMP_FATAL(AffinityInvalidMask, "kmp_get_affinity_mask_proc");
        }
    }

    if ((proc < 0) || ((unsigned)proc >= KMP_CPU_SETSIZE)) {
        return 0;
    }
    if (! KMP_CPU_ISSET(proc, fullMask)) {
        return 0;
    }

    return KMP_CPU_ISSET(proc, (kmp_affin_mask_t *)(*mask));
}


// Dynamic affinity settings - Affinity balanced
void __kmp_balanced_affinity( int tid, int nthreads )
{
    if( __kmp_affinity_uniform_topology() ) {
        int coreID;
        int threadID;
        // Number of hyper threads per core in HT machine
        int __kmp_nth_per_core = __kmp_avail_proc / __kmp_ncores;
        // Number of cores
        int ncores = __kmp_ncores;
        // How many threads will be bound to each core
        int chunk = nthreads / ncores;
        // How many cores will have an additional thread bound to it - "big cores"
        int big_cores = nthreads % ncores;
        // Number of threads on the big cores
        int big_nth = ( chunk + 1 ) * big_cores;
        if( tid < big_nth ) {
            coreID = tid / (chunk + 1 );
            threadID = ( tid % (chunk + 1 ) ) % __kmp_nth_per_core ;
        } else { //tid >= big_nth
            coreID = ( tid - big_cores ) / chunk;
            threadID = ( ( tid - big_cores ) % chunk ) % __kmp_nth_per_core ;
        }

        KMP_DEBUG_ASSERT2(KMP_AFFINITY_CAPABLE(),
          "Illegal set affinity operation when not capable");

        kmp_affin_mask_t *mask = (kmp_affin_mask_t *)KMP_ALLOCA(__kmp_affin_mask_size);
        KMP_CPU_ZERO(mask);

        // Granularity == thread
        if( __kmp_affinity_gran == affinity_gran_fine || __kmp_affinity_gran == affinity_gran_thread) {
            int osID = address2os[ coreID * __kmp_nth_per_core + threadID ].second;
            KMP_CPU_SET( osID, mask);
        } else if( __kmp_affinity_gran == affinity_gran_core ) { // Granularity == core
            for( int i = 0; i < __kmp_nth_per_core; i++ ) {
                int osID;
                osID = address2os[ coreID * __kmp_nth_per_core + i ].second;
                KMP_CPU_SET( osID, mask);
            }
        }
        if (__kmp_affinity_verbose) {
            char buf[KMP_AFFIN_MASK_PRINT_LEN];
            __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, mask);
            KMP_INFORM(BoundToOSProcSet, "KMP_AFFINITY", (kmp_int32)getpid(),
              tid, buf);
        }
        __kmp_set_system_affinity( mask, TRUE );
    } else { // Non-uniform topology

        kmp_affin_mask_t *mask = (kmp_affin_mask_t *)KMP_ALLOCA(__kmp_affin_mask_size);
        KMP_CPU_ZERO(mask);

        // Number of hyper threads per core in HT machine
        int nth_per_core = __kmp_nThreadsPerCore;
        int core_level;
        if( nth_per_core > 1 ) {
            core_level = __kmp_aff_depth - 2;
        } else {
            core_level = __kmp_aff_depth - 1;
        }

        // Number of cores - maximum value; it does not count trail cores with 0 processors
        int ncores = address2os[ __kmp_avail_proc - 1 ].first.labels[ core_level ] + 1;

        // For performance gain consider the special case nthreads == __kmp_avail_proc
        if( nthreads == __kmp_avail_proc ) {
            if( __kmp_affinity_gran == affinity_gran_fine || __kmp_affinity_gran == affinity_gran_thread) {
                int osID = address2os[ tid ].second;
                KMP_CPU_SET( osID, mask);
            } else if( __kmp_affinity_gran == affinity_gran_core ) { // Granularity == core
                int coreID = address2os[ tid ].first.labels[ core_level ];
                // We'll count found osIDs for the current core; they can be not more than nth_per_core;
                // since the address2os is sortied we can break when cnt==nth_per_core
                int cnt = 0;
                for( int i = 0; i < __kmp_avail_proc; i++ ) {
                    int osID = address2os[ i ].second;
                    int core = address2os[ i ].first.labels[ core_level ];
                    if( core == coreID ) {
                        KMP_CPU_SET( osID, mask);
                        cnt++;
                        if( cnt == nth_per_core ) {
                            break;
                        }
                    }
                }
            }
        } else if( nthreads <= __kmp_ncores ) {

            int core = 0;
            for( int i = 0; i < ncores; i++ ) {
                // Check if this core from procarr[] is in the mask
                int in_mask = 0;
                for( int j = 0; j < nth_per_core; j++ ) {
                    if( procarr[ i * nth_per_core + j ] != - 1 ) {
                        in_mask = 1;
                        break;
                    }
                }
                if( in_mask ) {
                    if( tid == core ) {
                        for( int j = 0; j < nth_per_core; j++ ) {
                            int osID = procarr[ i * nth_per_core + j ];
                            if( osID != -1 ) {
                                KMP_CPU_SET( osID, mask );
                                // For granularity=thread it is enough to set the first available osID for this core
                                if( __kmp_affinity_gran == affinity_gran_fine || __kmp_affinity_gran == affinity_gran_thread) {
                                    break;
                                }
                            }
                        }
                        break;
                    } else {
                        core++;
                    }
                }
            }

        } else { // nthreads > __kmp_ncores

            // Array to save the number of processors at each core
            int* nproc_at_core = (int*)KMP_ALLOCA(sizeof(int)*ncores);
            // Array to save the number of cores with "x" available processors;
            int* ncores_with_x_procs = (int*)KMP_ALLOCA(sizeof(int)*(nth_per_core+1));
            // Array to save the number of cores with # procs from x to nth_per_core
            int* ncores_with_x_to_max_procs = (int*)KMP_ALLOCA(sizeof(int)*(nth_per_core+1));

            for( int i = 0; i <= nth_per_core; i++ ) {
                ncores_with_x_procs[ i ] = 0;
                ncores_with_x_to_max_procs[ i ] = 0;
            }

            for( int i = 0; i < ncores; i++ ) {
                int cnt = 0;
                for( int j = 0; j < nth_per_core; j++ ) {
                    if( procarr[ i * nth_per_core + j ] != -1 ) {
                        cnt++;
                    }
                }
                nproc_at_core[ i ] = cnt;
                ncores_with_x_procs[ cnt ]++;
            }

            for( int i = 0; i <= nth_per_core; i++ ) {
                for( int j = i; j <= nth_per_core; j++ ) {
                    ncores_with_x_to_max_procs[ i ] += ncores_with_x_procs[ j ];
                }
            }

            // Max number of processors
            int nproc = nth_per_core * ncores;
            // An array to keep number of threads per each context
            int * newarr = ( int * )__kmp_allocate( sizeof( int ) * nproc );
            for( int i = 0; i < nproc; i++ ) {
                newarr[ i ] = 0;
            }

            int nth = nthreads;
            int flag = 0;
            while( nth > 0 ) {
                for( int j = 1; j <= nth_per_core; j++ ) {
                    int cnt = ncores_with_x_to_max_procs[ j ];
                    for( int i = 0; i < ncores; i++ ) {
                        // Skip the core with 0 processors
                        if( nproc_at_core[ i ] == 0 ) {
                            continue;
                        }
                        for( int k = 0; k < nth_per_core; k++ ) {
                            if( procarr[ i * nth_per_core + k ] != -1 ) {
                                if( newarr[ i * nth_per_core + k ] == 0 ) {
                                    newarr[ i * nth_per_core + k ] = 1;
                                    cnt--;
                                    nth--;
                                    break;
                                } else {
                                    if( flag != 0 ) {
                                        newarr[ i * nth_per_core + k ] ++;
                                        cnt--;
                                        nth--;
                                        break;
                                    }
                                }
                            }
                        }
                        if( cnt == 0 || nth == 0 ) {
                            break;
                        }
                    }
                    if( nth == 0 ) {
                        break;
                    }
                }
                flag = 1;
            }
            int sum = 0;
            for( int i = 0; i < nproc; i++ ) {
                sum += newarr[ i ];
                if( sum > tid ) {
                    // Granularity == thread
                    if( __kmp_affinity_gran == affinity_gran_fine || __kmp_affinity_gran == affinity_gran_thread) {
                        int osID = procarr[ i ];
                        KMP_CPU_SET( osID, mask);
                    } else if( __kmp_affinity_gran == affinity_gran_core ) { // Granularity == core
                        int coreID = i / nth_per_core;
                        for( int ii = 0; ii < nth_per_core; ii++ ) {
                            int osID = procarr[ coreID * nth_per_core + ii ];
                            if( osID != -1 ) {
                                KMP_CPU_SET( osID, mask);
                            }
                        }
                    }
                    break;
                }
            }
            __kmp_free( newarr );
        }

        if (__kmp_affinity_verbose) {
            char buf[KMP_AFFIN_MASK_PRINT_LEN];
            __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN, mask);
            KMP_INFORM(BoundToOSProcSet, "KMP_AFFINITY", (kmp_int32)getpid(),
              tid, buf);
        }
        __kmp_set_system_affinity( mask, TRUE );
    }
}

#endif // KMP_AFFINITY_SUPPORTED
