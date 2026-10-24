# SPDX-License-Identifier: GPL-2.0+
# Usage: emulate-first-stage.py STAGE_DIR OUT_DIR [case...]
# Runs the captured ROM, the built stage and the monitor entry under Unicorn.
from pathlib import Path
import os, sys, struct, json, hashlib, zlib, subprocess, time
root=Path(__file__).resolve().parents[4]
sys.path.insert(0,os.environ.get('ZX_EMULATION_DEPS',str(root/'tmp/v2801sg-emulation-deps')))
from unicorn import *
from unicorn.arm_const import *
from capstone import Cs,CS_ARCH_ARM,CS_MODE_ARM
from capstone.arm import ARM_OP_MEM
from unicorn import arm_const as uc_arm
stage=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
rom=(root/'tmp/v2801sg-stock-live-20261023/bootrom-low-4K.bin').read_bytes();combined=(stage/'combined-OFFLINE-NOT-FOR-FLASH.bin').read_bytes();monitor=(stage/'monitor.bin').read_bytes()
prefix=str(root/'staging_dir/toolchain-arm_cortex-a9_gcc-14.4.0_musl_eabi/bin/arm-openwrt-linux-muslgnueabi-')
syms={x.split()[2]:int(x.split()[0],16) for x in subprocess.check_output([prefix+'nm','-n',str(stage/'cold.elf')],text=True).splitlines() if len(x.split())==3}
expected=(Path(__file__).with_name('ddr-vendor.trace').read_text().splitlines())
def run(case='success',mode=2,initial_sctlr=0xc50078):
 u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
 u.mem_map(0,0x1000,UC_PROT_READ|UC_PROT_EXEC);u.mem_write(0,rom)
 u.mem_map(0x200000,0x10000);u.mem_map(0x40000000,0x2000000)
 for addr,size in [(0x94000000,0x1000),(0x94200000,0x1000),(0x94300000,0x3000),(0x94400000,0x8000),(0x9a100000,0x1000)]:u.mem_map(addr,size)
 u.cpr_write(15,0,1,0,0,0,False,initial_sctlr)
 u.reg_write(UC_ARM_REG_CPSR,0x600001d3);u.reg_write(UC_ARM_REG_SP,0x203000)
 flash=bytearray(combined)
 fault_injected=False
 instructions={i.address:i for i in Cs(CS_ARCH_ARM,CS_MODE_ARM).disasm((stage/'cold.bin').read_bytes(),0x200000)}
 detailed=Cs(CS_ARCH_ARM,CS_MODE_ARM);detailed.detail=True
 instructions={i.address:i for i in detailed.disasm((stage/'cold.bin').read_bytes(),0x200000)}
 if case=='crc':flash[8192+0x100]^=1
 regs={0x9430000c:0,0x94000038:mode<<4,0x94200000:0x403f1f00,0x94200004:0x3c0,0x94407008:3}
 uart=bytearray();peripheral_writes=[];transfers=[];current=None;ddr_events=[];counts={};stop={};stage_started=False;ddr_ready=False;handoff=False;stack_min=0x204000;dram_accesses=0
 def word(addr):return int.from_bytes(u.mem_read(addr,4),'little')
 def code(uc,addr,size,user):
  nonlocal stage_started,handoff,fault_injected
  if case=='fault' and syms['zx279125_ddr_init']<=addr<syms['zx_spi_wait'] and not fault_injected:
   ins=instructions[addr]
   if ins.mnemonic=='str' and ins.operands[1].type==ARM_OP_MEM:
    m=ins.operands[1].mem;reg=getattr(uc_arm,'UC_ARM_REG_'+ins.reg_name(m.base).upper());target=uc.reg_read(reg)+m.disp
    if target==0x94300080:
     fault_injected=True;old=uc.reg_read(UC_ARM_REG_CPSR);uc.reg_write(UC_ARM_REG_CPSR,(old&~31)|0x17|0xc0);uc.reg_write(UC_ARM_REG_SPSR,old);uc.reg_write(UC_ARM_REG_LR,addr+8)
     uc.cpr_write(15,0,5,0,0,0,False,8);uc.cpr_write(15,0,6,0,0,0,False,target);uc.reg_write(UC_ARM_REG_PC,uc.cpr_read(15,0,12,0,0,0,False)+16);return
  if addr==syms['zx279125_fault_halt'] and case=='fault':
   stop['reason']='fault halted';uc.emu_stop()
  elif addr==0x200000:
   stage_started=True
   assert bytes(uc.mem_read(addr,8192))==bytes(flash[:8192]),'ROM first-stage load mismatch'
  elif addr==syms['zx_ddr_delay']:ddr_events.append('D 2000')
  elif addr==0x40400000:
   handoff=True;assert bytes(uc.mem_read(addr,len(monitor)))==monitor
   assert word(0x202404)==5 and word(0x202408)==0
   assert uc.cpr_read(15,0,1,0,0,0,False)==(initial_sctlr&~0x2007)|0x1802
   assert uc.cpr_read(15,0,12,0,0,0,False)==syms['zx279125_early_vectors']
  elif addr==0x4040c22c:
   assert 0x406f0000<=uc.reg_read(UC_ARM_REG_SP)<0x40700000
   assert 0x406f0000<=uc.reg_read(UC_ARM_REG_R9)<0x40700000
   stop['reason']='board_init_f';uc.emu_stop()
  elif addr==0x158 and case in ('warm','icache','irq'):
   stop['reason']='warm refused';uc.emu_stop()
  elif addr==syms['halted']:
   stop['reason']='halted';uc.emu_stop()
 for addr in [syms['zx279125_fault_halt'],0x158,0x200000,syms['zx_ddr_delay'],0x40400000,0x4040c22c,syms['halted']]:u.hook_add(UC_HOOK_CODE,code,begin=addr,end=addr)
 if case=='fault':u.hook_add(UC_HOOK_CODE,code,begin=syms['zx279125_ddr_init'],end=syms['zx_spi_wait']-1)
 def mmio_read(uc,access,addr,size,value,user):
  nonlocal ddr_ready
  assert size==4,(hex(addr),size)
  counts[addr]=counts.get(addr,0)+1
  if addr in (0x9400001c,0x94000024,0x9400002c):v=0 if case=='pll' else 0x10000000
  elif addr==0x94300104:v=0 if case=='ddr104' else 1
  elif addr==0x94300100:
   v=0 if case=='ddr100' else 1
   if v:ddr_ready=True
  elif addr==0x94407008:v=regs.get(addr,0) if case=='spi_setup' and stage_started else regs.get(addr,0)|2
  elif addr==0x94407034:v=0x100 if current and current['read']<current['size'] and not(stage_started and (case=='spi_fifo' or (case=='spi_fifo_late' and len(transfers)>=18))) else 0
  elif addr==0x94407038:
   assert current and current['read']<current['size']
   off=current['offset']+current['read'];v=int.from_bytes(flash[off:off+4],'little');current['read']+=4
  elif addr==0x9440702c:v=0 if case=='spi_done' and stage_started else 1
  elif addr==0x94405014:v=0x10
  elif addr in regs:v=regs[addr]
  elif addr in (0x94407014,0x94407010,0x9440700c,0x94407004,0x94407024):v=0
  else:raise AssertionError(('unmodeled MMIO read',hex(addr),hex(uc.reg_read(UC_ARM_REG_PC))))
  uc.mem_write(addr,struct.pack('<I',v))
 def mmio_write(uc,access,addr,size,value,user):
  nonlocal current
  assert size==4
  pc=uc.reg_read(UC_ARM_REG_PC)
  peripheral_writes.append((addr,value))
  if syms['zx279125_ddr_init']<=pc<syms['zx_spi_wait']:ddr_events.append(f'W {addr:08x} {value:08x}')
  allowed={int(x.split()[1],16) for x in expected if x.startswith('W ')}|{0x94200000,0x94200004}|{0x94405000+x for x in (4,8,0x10,0x24,0x28,0x30,0x34,0x40,0x4c,0x50)}|{0x94407000+x for x in (4,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24,0x30)}
  assert addr in allowed,('unmodeled MMIO write',hex(addr),hex(pc))
  regs[addr]=value
  if addr==0x94407008 and case=='spi_setup' and stage_started:regs[addr]=value&~2
  if addr==0x94407020:assert value==3,('non-read SPI opcode',value)
  if addr==0x94407004 and value&1:
   current={'offset':regs[0x9440701c],'size':regs[0x94407018]+1,'read':0};transfers.append(current)
  if addr==0x94405004:
   uart.append(value&255)
   if mode in (0,15) and uart.endswith(b'ROM# '):stop['reason']='ROM console';uc.emu_stop()
 for addr,size in [(0x94000000,0x1000),(0x94200000,0x1000),(0x94300000,0x3000),(0x94400000,0x8000),(0x9a100000,0x1000)]:
  u.hook_add(UC_HOOK_MEM_READ,mmio_read,begin=addr,end=addr+size-1);u.hook_add(UC_HOOK_MEM_WRITE,mmio_write,begin=addr,end=addr+size-1)
 def sram_write(uc,access,addr,size,value,user):
  nonlocal stack_min
  if stage_started and addr>=0x203000:stack_min=min(stack_min,addr)
  assert addr+size<=0x204000,('SRAM write outside layout',hex(addr),size)
  if stage_started:assert addr>=0x202400,('first-stage code overwrite',hex(addr))
 u.hook_add(UC_HOOK_MEM_WRITE,sram_write,begin=0x200000,end=0x20ffff)
 def dram(uc,access,addr,size,value,user):
  nonlocal dram_accesses
  assert ddr_ready and word(0x202420)==8,('DDR access before initialization',hex(addr))
  dram_accesses+=1
 u.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,dram,begin=0x40000000,end=0x41ffffff)
 started=time.monotonic()
 start=0
 if case in ('warm','icache','irq'):
  regs[0x9430000c]=1;u.mem_write(0x200000,bytes(flash[:8192]));u.reg_write(UC_ARM_REG_SP,0x204000);u.reg_write(UC_ARM_REG_LR,0x158);start=0x200000
  if case=='icache':u.cpr_write(15,0,1,0,0,0,False,0xc51078)
  if case=='irq':u.reg_write(UC_ARM_REG_CPSR,0x600001d2);u.reg_write(UC_ARM_REG_SP,0x204000);u.reg_write(UC_ARM_REG_LR,0x158)
 try:u.emu_start(start,0x100000000,timeout=45000000,count=150000000)
 except UcError as e:raise RuntimeError((case,str(e),hex(u.reg_read(UC_ARM_REG_PC)),uart.decode('ascii','replace'))) from e
 trace=list(struct.unpack('<12I',u.mem_read(0x202400,48)))
 result={'case':case,'mode':mode,'stop':stop.get('reason'),'pc':hex(u.reg_read(UC_ARM_REG_PC)),'trace':[f'{x:08x}' for x in trace],'transfers':len(transfers),'handoff':handoff,'stack_min':hex(stack_min),'initial_sctlr':hex(initial_sctlr),'combined_sha256':hashlib.sha256(combined).hexdigest(),'monitor_sp':hex(u.reg_read(UC_ARM_REG_SP)),'uart':uart.decode('ascii','replace'),'seconds':round(time.monotonic()-started,3)}
 if result['stop']=='halted':
  assert result['uart'].endswith(f' FAIL {trace[1]:08x} {trace[2]:08x} {trace[8]:08x} {trace[7]:08x}\r\n'),result['uart']
  assert (' spi FAIL' in result['uart'])==(trace[1]>=3),result['uart']
 if case in ('warm','icache','irq'):
  assert result['stop']=='warm refused' and not peripheral_writes and not transfers and not dram_accesses
  assert u.reg_read(UC_ARM_REG_R0)==0xffffffff and u.reg_read(UC_ARM_REG_SP)==0x204000 and (u.reg_read(UC_ARM_REG_CPSR)&0x1ff)==(0x1d2 if case=='irq' else 0x1d3)
  assert u.cpr_read(15,0,1,0,0,0,False)==(0xc51078 if case=='icache' else 0xc50078) and u.cpr_read(15,0,12,0,0,0,False)==0
 elif case=='fault':
  fault=list(struct.unpack('<22I',u.mem_read(0x202500,88)));result['fault_record']=[f'{x:08x}' for x in fault]
  assert result['stop']=='fault halted' and fault_injected and not handoff and not dram_accesses
  assert fault[0]==0x46544c31 and fault[1]==4 and fault[4]==8 and fault[6]==0x94300080 and fault[2]&31==0x17 and fault[3]&31==0x13
  assert result['uart'].endswith(f'\r\nZXS FAULT 00000004 94300080 {fault[21]:08x}\r\n'),result['uart']
 elif mode in (0,15):assert result['stop']=='ROM console' and not stage_started and not ddr_events and not dram_accesses
 elif case=='success':
  assert result['stop']=='board_init_f' and handoff and trace[1]==5 and trace[2]==0
  assert result['uart'].startswith('SPI NOR\r\n\r\nZXS ddr spi ok\r\n'),result['uart']
  assert ddr_events==expected,(ddr_events,expected)
  assert transfers[0]=={'offset':0,'size':8192,'read':8192}
  assert len(transfers)==910
  for i,t in enumerate(transfers[1:]):assert t=={'offset':8192+i*256,'size':256,'read':256}
 elif case=='crc':assert result['stop']=='halted' and not handoff and trace[1]==4 and trace[2]==0xffffffb6
 else:
  assert result['stop']=='halted' and not handoff and trace[2]==0xffffff92
  if case=='spi_setup':assert trace[1]==3
  if case=='spi_fifo_late':assert trace[7]==0x1000
 (out/f'{case}-mode{mode}-sctlr{initial_sctlr:08x}.json').write_text(json.dumps(result,indent=2)+'\n');return result
if __name__=='__main__':
 cases=sys.argv[3:] or ['success']
 for case in cases:
  r=run(mode=int(case[-1],16)) if case in ('mode0','modef') else run(initial_sctlr=0x08c50078) if case=='alternate-sctlr' else run(case)
  print(json.dumps(r),flush=True)
