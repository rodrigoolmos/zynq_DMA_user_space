#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#define DMA_STATUS  0x4      /* R: Status (Ready/Busy) */
#define DMA_SA      0x18     /* R-W: Source address for the transaction */
#define DMA_DA      0x20     /* R-W: Destination address for the transaction */
#define DMA_BTT     0x28     /* R-W: Number of bytes to transfer */

#define DMA_SIZE    0x1000   // Size of the DMA AXI-Lite control port (as defined in the DTS and vivado)
#define MEM_SIZE    0x2000   // Size of the UIO reserved memory where the DMA will transfer data

#define PHYSICAL_DDR_ADDR   0x10000000  // Physical DDR address previously reserved in the DTS and mapped using UIO
#define BRAM_ADDR           0xC0000000  // Address provided by Vivado for the accelerator or desired hardware interface

// Write an array of 32-bit values to the base address + offset
void write_axi_lite_32b(void *addr, uint32_t offset, const uint32_t *value, uint32_t vector_elements) {
    for (uint32_t i = 0; i < vector_elements; i++){
        uint8_t *virt_addr = (uint8_t*) addr + offset + sizeof(uint32_t) * i;
        *((uint32_t *) virt_addr) = value[i];
    }
}

// Read an array of 32-bit values from the base address + offset
void read_axi_lite_32b(void *addr, uint32_t offset, uint32_t *value, uint32_t vector_elements) {
    for (uint32_t i = 0; i < vector_elements; i++){
        uint8_t *virt_addr = (uint8_t*) addr + offset + sizeof(uint32_t) * i;
        value[i] = *((uint32_t *) virt_addr);
    }
}

// Perform a DMA transaction by setting the source, destination, and the number of bytes to transfer
void dma_transaction(void *addr, uint32_t src_addr, uint32_t dest_addr, uint32_t n_bytes) {
    uint32_t busy = 0;

    write_axi_lite_32b(addr, DMA_DA, &dest_addr, 1);    // set destination addres
    write_axi_lite_32b(addr, DMA_SA, &src_addr, 1);     // set source addres
    write_axi_lite_32b(addr, DMA_BTT, &n_bytes, 1);     // set number of bytes

    // Wait until the transfer is complete (when bit 1 of the busy flag is set)
    do {
        read_axi_lite_32b(addr, DMA_STATUS, &busy, 1);
    } while (!(busy & 0x02));
}

int main() {
    int fd_dma, fd_mem, offset_half;
    int error = 0;

    // Map the DMA AXI-Lite control port
    fd_dma = open("/dev/uio0", O_RDWR);
    if (fd_dma < 0) {
        perror("Error opening /dev/uio0");
        return 1;
    }
    volatile uint32_t *ptr_dma = mmap(NULL, DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dma, 0);
    if (ptr_dma == MAP_FAILED) {
        perror("Error mapping DMA memory");
        close(fd_dma);
        return 1;
    }

    // Map the physical/virtual memory for the transfer
    fd_mem = open("/dev/uio2", O_RDWR);
    if (fd_mem < 0) {
        perror("Error opening /dev/uio2");
        munmap((void*)ptr_dma, DMA_SIZE);
        close(fd_dma);
        return 1;
    }
    volatile uint64_t *ptr_mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_mem, 0);
    if (ptr_mem == MAP_FAILED) {
        perror("Error mapping memory");
        munmap((void*)ptr_dma, DMA_SIZE);
        close(fd_dma);
        close(fd_mem);
        return 1;
    }

    // Initialize the first half of the test memory
    uint32_t num_elements = MEM_SIZE / (2 * sizeof(uint64_t));
    for (uint32_t i = 0; i < num_elements; i++) {
        ptr_mem[i] = i;
    }

    // DMA transactions:
    // 1. From physical DDR to BRAM
    dma_transaction((void*)ptr_dma, PHYSICAL_DDR_ADDR, BRAM_ADDR, MEM_SIZE / 2);
    // 2. From BRAM back to DDR (to the second half)
    dma_transaction((void*)ptr_dma, BRAM_ADDR, PHYSICAL_DDR_ADDR + MEM_SIZE / 2, MEM_SIZE / 2);

    // Compare the sent and received data
    offset_half = MEM_SIZE / (2 * sizeof(uint64_t));
    for (int i = 0; i < offset_half; i++) {
        if (ptr_mem[i] != ptr_mem[i + offset_half]) {
            error = 1;
            printf("Value error: sent %llu, received %llu\n", ptr_mem[i], ptr_mem[i + offset_half]);
        } else {
            printf("Sent %llu, received %llu\n", ptr_mem[i], ptr_mem[i + offset_half]);
        }
    }

    printf("DMA transaction test, status = %s\n", error ? "ERROR" : "OK");

    // Unmap and close the file descriptors
    munmap((void*)ptr_dma, DMA_SIZE);
    close(fd_dma);
    munmap((void*)ptr_mem, MEM_SIZE);
    close(fd_mem);

    return 0;
}
