all: myos.iso

boot.o: boot.s
	as --32 boot.s -o boot.o

kernel.o: kernel.c
	gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel.c -o kernel.o

mouse.o: mouse.c
	gcc -m32 -ffreestanding -O2 -Wall -Wextra -c mouse.c -o mouse.o

myos.bin: boot.o kernel.o mouse.o linker.ld
	ld -m elf_i386 -T linker.ld -nostdlib boot.o kernel.o mouse.o -o myos.bin

myos.iso: myos.bin
	mkdir -p isodir/boot/grub
	cp myos.bin isodir/boot/myos.bin
	printf 'menuentry "macOS 26 Style Custom OS" {\n\tmultiboot /boot/myos.bin\n}\n' > isodir/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso isodir

run: myos.iso
	qemu-system-i386 -cdrom myos.iso -netdev user,id=n1 -device ne2k_isa,netdev=n1,irq=11,iobase=0x300

clean:
	rm -rf *.o *.bin *.iso isodir
