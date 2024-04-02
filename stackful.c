/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Matthew Lee
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 编译
// gcc -m32 stackful.c stackful.s

const int CTX_SIZE = 1024;

// *(ctx + CTX_SIZE - 1) 存储 return address
// *(ctx + CTX_SIZE - 2) 存储 ebx
// *(ctx + CTX_SIZE - 3) 存储 edi
// *(ctx + CTX_SIZE - 4) 存储 esi
// *(ctx + CTX_SIZE - 5) 存储 ebp
// *(ctx + CTX_SIZE - 6) 存储 esp
char **MAIN_CTX;
char **NEST_CTX;
char **FUNC_CTX_1;
char **FUNC_CTX_2;

// 用于模拟切换协程的上下文
int YIELD_COUNT;

// 切换上下文，具体参见 stackful.s 的注释
extern void swap_ctx(char **current, char **next);

// 注意 x86 的栈增长方向是从高位向低位增长的，所以寻址是向下偏移的
char **init_ctx(char *func) {
    // 动态申请 CTX_SIZE 内存用于存储协程上下文
    size_t size = sizeof(char *) * CTX_SIZE;
    char **ctx = (char**) malloc(size);
    memset(ctx, 0, size);

    // 将 func 的地址作为其栈帧 return address 的初始值，
    // 当 func 第一次被调度时，将从其入口处开始执行
    *(ctx + CTX_SIZE - 1) = (char *) func;

    // https://github.com/mthli/blog/pull/12
    // 需要预留 6 个寄存器内容的存储空间，
    // 余下的内存空间均可以作为 func 的栈帧空间
    
    // 无参数的情况
    // *(ctx + CTX_SIZE - 6) 存储 esp
    // 此时针对无参函数*(ctx + CTX_SIZE - 7) 就是当前函数的栈顶
    // *(ctx + CTX_SIZE - 6) = (char *) (ctx + CTX_SIZE - 7);

    // 有参数的情况
    // 地址从高到低，在ctx + CTX_SIZE - 6下的地址，就是当前函数的栈
    // 函数的第一个参数应该存储在*(ctx + CTX_SIZE - 7)
    // 函数的第二个参数应该存储在*(ctx + CTX_SIZE - 8)
    // 后续参数依次类推
    *(ctx + CTX_SIZE - 7) = 10;

    // 这里很重要，现在只有一个参数，按理说栈顶应该在(ctx + CTX_SIZE - 8)的位置，但是下面代码确实偏移了9。
    // 1. 首先说明为什么栈顶在(ctx + CTX_SIZE - 8)，因为(ctx + CTX_SIZE - 8)内存地址是函数栈顶，同时栈顶会存储函数的return address
    // 也就是说该地址的4个字节(默认都是从低地址到高地址)存储函数的return address, 当协程上下文切换时，会将esp寄存器指向内存地址的4字节改为另一个函数的地址，因此需要return address预留4字节，栈顶自然就需要多偏移一位了。
    // 2. 根据刚才的分析栈顶只需要在最后一个参数的下面(栈从高地址向低地址增长)即可，那为什么实际使用的确实(ctx + CTX_SIZE - 9)呢？这多出来的4个字节(多减了一个1，单位是char*，当前是32位环境，因此是4字节)是什么？
    // 根据c语言32位__cdecl调用约定，调用方将函数参数从右至左依次压栈(注意是32位，如果是64位参数少于7个时会使用寄存器传参)，被调用方则需要将调用方函数的栈底指针ebp压栈(以便函数返回时恢复)，同时将栈底指针赋值为调用方的栈顶指针，即更新一下新的函数栈，这里好理解。
    // 上述操作结束后，就正式进入被调用函数的代码逻辑了，在被调用函数中访问函数参数，本质就是方案上一个函数的栈空间(参数是调用方压栈的，自然算在调用方的函数栈)，因此在被调用函数中是通过ebp向高地址偏移得到函数参数的，如果是被调用函数中定义的局部变量，此时就应该属于被调用函数的栈，那么就应该使用ebp向低地址偏移。
    // 理解了32为C语言调用约定中，函数参数属于上一个函数栈空间后，现在带入实际情况就很好理解了，我们现在有一个参数，因此在调用方会将参数压栈(当前参数假设时一个int，占用4字节)，这里还有关键的一步，在进入被调用函数前，call指令本身会将下一条指令的汇编地址压栈，作为函数的return address，即函数返回后下一条指令的地址，被调用函数返回时，是将ebp指向内存内容的4个字节地址作为eip指令指针寄存器的值，然后进入被调用函数后，还会将ebp再次压栈以便被调用函数返回时恢复(ebp入栈占用4个字节)，此时只需要将ebp-8后的4个字节就是函数的第一个参数。这8字节的低4位是被调用函数压入的ebp，即调用方的栈底，高4位是函数的return address。
    // (ctx + CTX_SIZE - 9)相比第一个参数ctx + CTX_SIZE - 7, 多偏移的2个4字节, 从高地址到低地址分别是:
    // * 预留给函数上下文切换时ret指令会弹出一次return address
    // 被调用方获取参数时会偏移2次4字节，一次是被调用方自己压入的ebp，另一次是在进入被调用方函数之前call函数会压入return address(虽然此时return address在我们的实现中可能为0?)
    *(ctx + CTX_SIZE - 6) = (char *) (ctx + CTX_SIZE - 9);

    // *(ctx + CTX_SIZE - 6) = (char *) (ctx + CTX_SIZE - 6);
    // *(ctx + CTX_SIZE - 7) = 10;
    // *(ctx + CTX_SIZE - 5) = (char *) (ctx + CTX_SIZE - 6);

    return ctx + CTX_SIZE;
}

// 因为我们只有 4 个协程（其中一个是主协程），
// 所以这里简单用 switch 来模拟调度器切换上下文了
void yield() {
    switch ((YIELD_COUNT++) % 4) {
    case 0:
        swap_ctx(MAIN_CTX, NEST_CTX);
        break;
    case 1:
        swap_ctx(NEST_CTX, FUNC_CTX_1);
        break;
    case 2:
        swap_ctx(FUNC_CTX_1, FUNC_CTX_2);
        break;
    case 3:
        swap_ctx(FUNC_CTX_2, MAIN_CTX);
        break;
    default:
        // DO NOTHING
        break;
    }
}

void nest_yield() {
    yield();
}

void nest(int val) {
    // 随机生成一个整数作为 tag
    int tag = rand() % 100;
    for (int i = 0; i < 3; i++) {
	int a = 1;
	int b = 2;
	int c = 3;
	int d = val;
        printf("nest, tag: %d, index: %d, val: %d\n", tag, i, val);
        nest_yield();
    }
}

void func() {
    // 随机生成一个整数作为 tag
    int tag = rand() % 100;
    for (int i = 0; i < 3; i++) {
        printf("func, tag: %d, index: %d\n", tag, i);
        yield();
    }
}

int main() {
    MAIN_CTX = init_ctx((char *) main);

    // 证明 nest() 可以在其嵌套函数中被挂起
    NEST_CTX = init_ctx((char *) nest);

    // 证明同一个函数在不同的栈帧空间上运行
    FUNC_CTX_1 = init_ctx((char *) func);
    FUNC_CTX_2 = init_ctx((char *) func);

    int tag = rand() % 100;
    for (int i = 0; i < 3; i++) {
        printf("main, tag: %d, index: %d\n", tag, i);
        yield();
    }

    free(MAIN_CTX - CTX_SIZE);
    free(NEST_CTX - CTX_SIZE);
    free(FUNC_CTX_1 - CTX_SIZE);
    free(FUNC_CTX_2 - CTX_SIZE);
    return 0;
}
