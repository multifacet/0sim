# 0sim Usage Guide

**NOTE: This is the 0sim users guide. To see the original Linux kernel README,
look at [LINUX-README](./LINUX-README).**

0sim is consists mostly of a modified Linux kernel. This kernel allows a host
to run very large-memory guest VMs. This guide assumes that you have the 0sim
kernel installed on the host machine already. It walks through some of the
tricks and useful tips for using 0sim to make measurements.

## Virtual Machines and Hypervisors

So far, we have been using QEMU/KVM as our hypervisor (the software than runs
and manages virtual machines). In principle, there is nothing to stop us from
using any other hypervisor, but we just happen to be using KVM.

## Vagrant

While one can run VMs directly via the QEMU/KVM command-line tools, this gets
messy/annoying pretty quickly. Instead, I recommend using an Environment
Manager, software that helps you quickly build and run reproducible
environments. The one I have been using is called Vagrant.

Vagrant builds and boots VMs using a per-VM configuration file called
`Vagrantfile`. This config file is written in Ruby, but you don't actually have
to know any Ruby to use it (I don't). You can most likely just copy my config
and modify it as needed.

Vagrant allows extensive customization of VMs and it is extensible by plugins
too. Some important configuration options that I have found useful:

- Choosing the `provider`: In Vagrant, each VM has a "provider", which is
  basically the hypervisor. In particular, we will choose `libvirt`, which
  allows us to use KVM.
- Choosing the base `box`: In Vagrant, each VM is based off of a template
  called a `box`. This allows you to avoid the annoyance of having to build a
  VM image, which is very nice.
- Number of vCPUs.
- Amount of memory (RAM).
- Networking options: Vagrant allows you to connect the VM to the host network
  interface, so that you have network access from the VM.
- Port forwarding: This allows you to forward traffic from some port on the
  host to some port in the guest. For example, I usually forward port 5555 on
  the host to port 22 on the guest, allowing me to SSH directly to VM.
- Shared folders: Vagrant allows you to share a directory between the host and
  the guest. Usually this is done by exposing the host directory as an NFS file
  system mounted in the guest. We have been using this mechanism to get
  simulation results out of the VM.
- SSH key: Vagrant allows you to point to your public key. This allows you to
  SSH into the VM without entering a password.

### Installing

To install vagrant, use the packages here:

    https://www.vagrantup.com/downloads.html

To be able to SSH into a VM, you will need to create a key-pair. Run:

    ssh-keygen

You will be prompted for a bunch of things. I recommend just taking the
defaults, unless you know what you are doing.

Support for libvirt is provided by a plugin called `vagrant-libvirt`. See the
installation notes here:

    https://github.com/vagrant-libvirt/vagrant-libvirt#installation

### 0sim with KVM/libvirt/vagrant

You will need to have the following working:
- Vagrantfile and shared directories should exist on the host
- You need to have KVM/QEMU installed
- You need to have libvirt install and your username needs to be part of the
  `libvirtd` group.
- For vagrant to correctly start an NFS server, the user needs to have sudo
  priveleges to run some commands: see

      https://www.vagrantup.com/docs/synced-folders/nfs.html#root-privilege-requirement

  For convenience, I recommend adding these command as NOPASSWD for the given
  users in the /etc/sudoers file.

### Usage

In order to boot large-memory VMs, you will need to have memory or storage to
back the VM. In particular, we use a large swap device. You can enable and
disable a swap device with `swapon` and `swapoff`:

    sudo swapon /dev/sdb
    sudo swapoff /dev/sda5

I recommend putting your Vagrantfile and shared directory in their own
directory on the host. Otherwise, things get messy fast.

vagrant/
  |- Vagrantfile
  `- vm_shared/
       `- results/

To build or run the VM:

    cd vagrant/
    vagrant up

The first time you do this, it may take a while, as it will creat the VM first.
Subsequently, when you do `vagrant up`, it will just boot the VM. Depending on
how much memory you give the VM, this may take a while.

To SSH into the VM from the host, use

    cd vagrant/
    vagrant ssh

I recommend adding your normal SSH key to the `authorized_keys` file of the
`vagrant` user in the VM, so that you can SSH directly to the VM without having
to go through the host.

## libvirt and `virsh`

You will want to pin you vCPUs to fixed host CPUs when running benchmarks. To do
this, use `virsh`.

To list all running VMs:

    virsh list

To find out where a vCPU is currently running, run:

    virsh vcpuinfo <vm_name>

To pin a vCPU:

    virsh vcpupin <vm_name> <vcpu #> <cpu #>

## Zswap settings

When running your simulations, you will need to enable Zswap and fix some
parameters:

Set the maximum amount of memory Zswap can use (e.g. to 50% of host main
memory):

    echo 50 | sudo tee /sys/module/zswap/parameters/max_pool_percent

Set the zpool to `ztier`:

    echo ztier | sudo tee /sys/module/zswap/parameters/zpool

Enable Zswap:

    echo 1 | sudo tee /sys/module/zswap/parameters/enabled

You will need to re-set these every time you reboot.

You can view some Zswap metrics by running:

    sudo tail /sys/kernel/debug/zswap/*

## CPU frequency

You will want to set the scaling governor to `performance` using the cpupower
program.

    sudo cpupower frequency-set -g performance

However, you will need to compile cpupower from source:

    cd 0sim/tools/power/cpupower
    make

## Running Simulations

Finally, how do we run simulations? When you SSH into the VM, you should be
able to use your VM as if it had a huge amount of memory. You will run your
experimental workload in the VM, outputting any measurements you want to
gather.

I recommend that your workload should print measurements to stdout. You can
then redirect this output somewhere else. In particular, I recommend redirecting
output to a file on one of the shared directories you create through Vagrant.

### Caveats

- Ubuntu only supports up to 1023GB of RAM. If you use 1024, or any greater
  value, your VM will hang at boot time.
