#include <iostream>

#include "corundum.h"
#include "dma.h"
#include "mem.h"

void DMAReader::step()
{
    p.dma_ready = 1;
    if (p.dma_valid) {
        DMAOp *op = new DMAOp;
        op->engine = this;
        op->dma_addr = p.dma_addr;
        op->ram_sel = p.dma_ram_sel;
        op->ram_addr = p.dma_ram_addr;
        op->len = p.dma_len;
        op->tag = p.dma_tag;
        op->write = false;
        pending.insert(op);
        /*std::cout << "dma[" << label << "] op " << op->dma_addr << " -> " <<
            op->ram_sel << ":" << op->ram_addr <<
            "   len=" << op->len << "   tag=" << (int) op->tag << std::endl;*/

        pci_dma_issue(op);
    }

    p.dma_status_valid = 0;
    if (!completed.empty()) {
        DMAOp *op = completed.front();
        completed.pop_front();

        //std::cout << "dma[" << label << "] status complete " << op->dma_addr << std::endl;

        p.dma_status_valid = 1;
        p.dma_status_tag = op->tag;
        pending.erase(op);
        delete op;
    }
}

void DMAReader::pci_op_complete(DMAOp *op)
{
    mw.op_issue(op);
}

void DMAReader::mem_op_complete(DMAOp *op)
{
    completed.push_back(op);
    //std::cout << "dma[" << label << "] mem complete " << op->dma_addr << std::endl;
}
