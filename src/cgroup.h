/*
 * cgroup.h: Interface to tools for managing cgroups
 *
 * Copyright IBM Corp. 2008
 *
 * See COPYING.LIB for the License of this software
 *
 * Authors:
 *  Dan Smith <danms@us.ibm.com>
 */

#ifndef CGROUP_H
#define CGROUP_H

struct virCgroup;
typedef struct virCgroup *virCgroupPtr;

int virCgroupForDriver(const char *name,
                       virCgroupPtr *group,
                       int privileged,
                       int create);

int virCgroupForDomain(virCgroupPtr driver,
                       const char *name,
                       virCgroupPtr *group,
                       int create);

int virCgroupAddTask(virCgroupPtr group, pid_t pid);

int virCgroupSetMemory(virCgroupPtr group, unsigned long kb);

int virCgroupDenyAllDevices(virCgroupPtr group);

int virCgroupAllowDevice(virCgroupPtr group,
                         char type,
                         int major,
                         int minor);
int virCgroupAllowDeviceMajor(virCgroupPtr group,
                              char type,
                              int major);

int virCgroupSetCpuShares(virCgroupPtr group, unsigned long long shares);
int virCgroupGetCpuShares(virCgroupPtr group, unsigned long long *shares);

int virCgroupGetCpuacctUsage(virCgroupPtr group, unsigned long long *usage);

int virCgroupRemove(virCgroupPtr group);

void virCgroupFree(virCgroupPtr *group);

#endif /* CGROUP_H */
