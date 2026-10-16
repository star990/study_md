# GPU 与 AI 算力

> **系列**：`08-ai-soc`  
> **前置**：[`01-soc-common.md`](01-soc-common.md)  
> **相关**：[`02-tpu.md`](02-tpu.md)（**对照主文已写**：SIMT vs 数据流、延迟隐藏 vs 显式搬运）

聚焦 **GPU** 在 AI SoC / 训练推理场景中的架构要点：SIMT、层级存储、与主机/其它加速器协同。  
与 TPU 的系统级对照优先读 [`02-tpu.md` §5](02-tpu.md#5-与-gpu-最主要的区别)；本文后续补 GPU 侧展开。

**读完应能**：
- 用 warp/wavefront + SIMT 解释 GPU 为何适合大规模并行算子
- 说清寄存器 / shared / L2 / HBM(GDDR) 层次与 bank 冲突直觉
- 理解 Tensor Core / 矩阵单元相对传统 CUDA core 的位置
- 对照 TPU：灵活性、生态与能效的取舍

---

## 大纲（待成文）

1. GPU 在 AI SoC 中的角色与软件栈
2. SIMT、warp、占用率与分支发散
3. 显存 / cache / shared memory
4. Tensor 单元、多卡/片间互联与 SoC 集成

状态：计划。
