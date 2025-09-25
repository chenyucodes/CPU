# DMA相关知识及问题

## DMA是什么？

在传统的计算机系统中，外设设备需要通过CPU来控制数据的传输。当外设设备需要读取或写入数据时，需要向CPU发出请求，CPU则负责处理这些请求和数据传输的操作。这种方式会占用CPU的时间和资源，降低计算机系统的整体性能。
DMA控制器是一个特殊的硬件设备，它可以直接和系统内存进行数据传输，而不需要通过CPU来控制。在数据传输过程中，外设设备会向DMA控制器发送请求，告诉它需要读取或写入的数据的地址和大小。然后DMA控制器会直接从内存中读取或写入数据，完成数据传输的过程。这样就可以减少CPU的负担，提高数据传输的速度和效率。

## 相关配置

在HAL库的初始化中，DMA的全满中断默认使能，实际上半满中断也是默认使能的，感兴趣的可以看看函数
HAL_UART_Receive_DMA(UART_HandleTypeDef *huart, uint8_t *pData, uint16_t Size) 的代码。

外设->内存 模式下，如果是直传模式，每当有数据被外设送入到FIFO，这些数据会直接从FIFO送到目标地址存储。如果是FIFO模式，FIFO中的数据会等待中断再被传输：


![img](https://i-blog.csdnimg.cn/direct/c8986b8c3db2426abd18e051cc5841b6.png)

## DMA工作模式

### 【1】直传模式（Direct Mode）

直传模式下的**普通模式**和**循环模式**，半满和全满中断都是按照用户配置的"接收数据长度"（也就是**NDTR**）正常产生。

### **【2】FIFO模式（FIFO Mode）**

友情提醒
这部分的内容有点复杂，本人实际应用也没有使用上，只是学习时顺带研究调试了一下，本意只是大概搞明白和Direct Mode的基本区别，没想到疑惑一个接一个，很多地方用户手册也没说明白，最后居然记录了这么多！我将这部分的学习成果分享出来，希望其他使用FIFO Mode的朋友遇到问题了有个参考。不感兴趣的可以直接忽略这部分。

介绍
在两边数据流数据宽度不一致的情况下，需要设置FIFO Mode（或叫"突发模式"）。
当数据宽度不一致，一方的单份数据可能是另一方的多份数据，若使用直传模式每次都从FIFO中搬运同样长度的数据，一定会出现数据过载或欠载的情况。因此突发模式通过在FIFO中控制输出单份数据的长度，避免数据流出错。
每个数据流配置有一个4字即16字节的FIFO，在突发模式下，通过配置溢出阈值、数据宽度、节拍数来设置每次在FIFO中搬运的数据长度。

Burst Size (节拍数)：每次在FIFO搬运数据的份数；
Data Width (数据宽度)：每份被搬运数据的字长；
Threshold (FIFO阈值)：FIFO产生中断所需达到的阈值（1/4满【4字节】、半满【8字节】、3/4满【12字节】、全满【16字节】）。
显而易见的是需要保证搬运数据时FIFO中的数据长度满足该次搬运的数据长度要求，否则数据流就可能出错，这意味着中断时FIFO中的数据长度需要是（Burst Size × Data Width）的倍数，不满足要求的配置是被禁止的：

![FIFO](https://i-blog.csdnimg.cn/direct/7293b1af3a9640d791d12eabefbb97ce.png)

ps:上图表格没有写出MBURST=single tranfer的情况（即每份数据都会被立即传输），因为这与直传模式一样，事实上直传模式下MBURST会由硬件强制置成single tranfer。
举个例子，若数据宽度设置为Byte，溢出阈值设置为3/4，那每当FIFO缓冲12字节数据就会产生中断请求搬运数据到目标地址：

若Burst Size设为4，那就是一次搬运(4 × 1Byte = 4Bytes)，也就是一次中断会发生12/4 = 3份数据传输，是不会导致数据流出错的；
但若Burst Size设为8，12字节数据无法分成2份被搬运，因此这种配置是被禁止的。
下图非常好地描述了突发模式中FIFO对数据的存储和输送：

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/bab93c68f7134862bec2ee10a19dab61.png)

### 普通模式下

当**NDTR变为0**时或达到**FIFO阈值**时都会触发中断，半满中断和全满中断的标志位都有可能为1，下文会详细分析。无论哪种中断标志位组合都会导致禁止中断使能（非循环模式下，HAL库的中断处理函数中会把中断标志位为1的中断类型禁掉）：

```c
/* Half Transfer Complete Interrupt management ******************************/
  if ((tmpisr & (DMA_FLAG_HTIF0_4 << hdma->StreamIndex)) != RESET)
  {...}
/* Disable the half transfer interrupt if the DMA mode is not CIRCULAR */
  else
  {...}

```

```c
 /* Transfer Complete Interrupt management ***********************************/
  if ((tmpisr & (DMA_FLAG_TCIF0_4 << hdma->StreamIndex)) != RESET)
  {...}
  /* Disable the transfer complete interrupt if the DMA mode is not CIRCULAR */
  else
  {...}

```

此时需要重置NDTR，否则不会再产生任何中断。由于HAL库的中断处理函数会自动清除标志位，重新使能中断；我们只需在回调函数中再次调用`HAL_UART_Receive_DMA()`即可重置NDTR。

### FIFO Mode具体分析

看到这里大家可能会有种违和感，FIFO Mode的中断理应是由每个Stream的FIFO配置的"FIFO阈值"决定的，而FIFO大小是固定16字节的，也就是说触发中断的阈值也是几个固定的数，这个过程中和用户自己设置的NDTR又有什么关联？
下面列几个FIFO Mode中循环模式下的实验现象，数据宽度均为Byte（黄色标注为发生中断）：

#### 【1】FIFO Threshold：3/4 ，NDTR：14，此时FIFO阈值 = 12 > 7 = 1/2 NDTR：

| 接收数据长度 | TCIF(全满中断标志) | HTIF(半满中断标志) | NDTR |
| ------------ | ------------------ | ------------------ | ---- |
| 0            | 0                  | 0                  | 14   |
| 4            | 0                  | 0                  | 10   |
| 8            | 0                  | 0                  | 6    |
| 12           | 0                  | 1                  | 2    |
| 16           | 0                  | 0                  | 12   |
| 20           | 0                  | 0                  | 8    |
| 24           | 1                  | 1                  | 4    |
| …            |                    |                    |      |
| 36           | 1                  | 1                  | 6    |
| …            |                    |                    |      |
| 48           | 1                  | 0                  | 8    |

#### 【2】**FIFO Threshold：1/4 ，NDTR：6，此时FIFO阈值 = 4 > 3 = 1/2 NDTR：**

| 接收数据长度 | TCIF(全满中断标志) | HTIF(半满中断标志) | NDTR |
| ------------ | ------------------ | ------------------ | ---- |
| 0            | 0                  | 0                  | 6    |
| 4            | 0                  | 1                  | 2    |
| 8            | 1                  | 0                  | 4    |
| 12           | 1                  | 1                  | 6    |
| 16           | 0                  | 1                  | 2    |
| 20           | 1                  | 0                  | 4    |
| 24           | 1                  | 1                  | 6    |

#### 【3】**FIFO Threshold：1/4 ，NDTR：10，此时FIFO阈值 = 4 < 5 = 1/2 NDTR：**

| 接收数据长度 | TCIF(全满中断标志) | HTIF(半满中断标志) | NDTR |
| ------------ | ------------------ | ------------------ | ---- |
| 0            | 0                  | 0                  | 10   |
| 4            | 0                  | 0                  | 6    |
| 8            | 0                  | 1                  | 2    |
| 12           | 1                  | 0                  | 8    |
| 16           | 0                  | 1                  | 4    |
| 20           | 1                  | 0                  | 10   |
| 24           | 0                  | 0                  | 6    |
| 28           | 0                  | 1                  | 2    |

只看前两个实验现象的话，虽然确实触发中断了，但可能对两个标志位的值一头雾水；但对照第三个实验数据就一目了然了：实验三中接收数据长度为4和24时，即使长度达到FIFO阈值了却仍因为两个中断标志位都为0而无法触发中断。说明即使在FIFO Mode，两个中断标志位仍是由NDTR决定的！
FIFO Mode下，半满和全满标志位仍会因为NDTR变为初始值一半和0时“变化”，之所以带“引号”是因为标志位的值不会实时变化反映出来，即使到了半满和全满，Debug里看标志位也还是0。但当FIFO中数据达到FIFO阈值时，MCU会判断之前记录的半满和全满标志位是不是为1，如果有任意一个为1则触发中断。可以说，FIFO阈值是触发中断的第一条件，半满全满中断标志位是触发中断的第二条件。

------



## DMA接收示例

下面以Direct Mode、循环模式下一个8字节大小的接收缓冲区示例，单片机串口接收完后打印缓冲区内的数据：

**1、初始化后，NDTR=8：**

![img](https://i-blog.csdnimg.cn/direct/a0eb8f7f6b284cf281b5280556916eb6.png)

**2、接收到1字节数据后，NDTR=7：**

![3](https://i-blog.csdnimg.cn/direct/dc2fbe93a85443d4a0f088b72c7ae833.png)

**3、再接收到2字节数据后，NDTR=5：**

![4](https://i-blog.csdnimg.cn/direct/2919da6bc6fc49aa9b25416f7b461346.png)

**4、当缓冲区被写满后，即NDTR为0时会触发全满中断。普通模式下需手动清除标志位；循环模式下NDTR会自动重置（到0时自动重置回缓冲区长度），写指针回到缓冲区起始地址，且此时会触发中断回调函数：**

![img](https://i-blog.csdnimg.cn/direct/03d4c26053064f4cb24f1b2201a9feb8.png)

**5、再接收到数据时，将从缓冲区起始位置重新开始写入**

![6](https://i-blog.csdnimg.cn/direct/d5cefbd76726494d88dbb44ea19f8da4.png)

------

## DMA串口接收+ idle 中断踩坑指南

DMA接收的好处不再赘述，缺点是DMA需要FIFO阈值达到设定值才能触发中断，如果接收的数据少于这个阈值，MCU就无法通知我们缓冲区的数据更新了。因此空闲中断就派上用场了：每当串口接收完一个字节数据后检测到下一个bit不为起始位，说明接收完连续的一帧数据了，就会产生空闲中断。

![请添加图片描述](https://i-blog.csdnimg.cn/img_convert/7ff87aba87ceea92e227dcc1b975a498.png)



### 1、为什么 普通模式（正常模式）下 接收完一次数据DMA就不工作了？

#### 正常模式下：

接下来看一下为什么**普通模式**下，接收完一次数据DMA就不工作了：
在**普通模式**下触发空闲中断时，HAL库串口中断函数中会自动进行配置（已把不相关的代码省去）

```c
void HAL_UART_IRQHandler(UART_HandleTypeDef *huart)
{
		/* --- 无关的已省略 --- */
		
 /* Check current reception Mode :
     If Reception till IDLE event has been selected : */
  if ((huart->ReceptionType == HAL_UART_RECEPTION_TOIDLE)
      && ((isrflags & USART_SR_IDLE) != 0U)
      && ((cr1its & USART_SR_IDLE) != 0U))
  {
    __HAL_UART_CLEAR_IDLEFLAG(huart);			

	/* Check if DMA mode is enabled in UART */
	if (HAL_IS_BIT_SET(huart->Instance->CR3, USART_CR3_DMAR))
	{
	   /* DMA mode enabled */
	   uint16_t nb_remaining_rx_data = (uint16_t) __HAL_DMA_GET_COUNTER(huart->hdmarx);
	   if ((nb_remaining_rx_data > 0U) && (nb_remaining_rx_data < huart->RxXferSize))
	      {
	      /* In Normal mode, end DMA xfer and HAL UART Rx process*/
	        if (huart->hdmarx->Init.Mode != DMA_CIRCULAR)
	        {
				/* Disable the DMA transfer for the receiver request by resetting the DMAR bit in the UART CR3 register */
		        ATOMIC_CLEAR_BIT(huart->Instance->CR3, USART_CR3_DMAR);
		          
				/* At end of Rx process, restore huart->RxState to Ready */
	          	huart->ReceptionType = HAL_UART_RECEPTION_STANDARD;
	          	 
				ATOMIC_CLEAR_BIT(huart->Instance->CR1, USART_CR1_IDLEIE);
				
				/* Last bytes received, so no need as the abort is immediate */
	          	(void)HAL_DMA_Abort(huart->hdmarx);
			}	
			   /*Call legacy weak Rx Event callback*/
   			   HAL_UARTEx_RxEventCallback(huart, (huart->RxXferSize - huart->RxXferCount));
	      }		
	   }
   }
}	//end of function

```

需要关注的几个点：

```
1、首先判断RxEventType是否为HAL_UART_RECEPTION_TOIDLE，且处理函数会自动清空IDLE标志；
2、DMAR置0导致了禁止DMA Mode的接收；
3、ReceptionType从HAL_UART_RECEPTION_TOIDLE变为了HAL_UART_RECEPTION_STANDARD，这会导致下次接收无法进入该段处理程序；
4、IDLEIE置0导致了空闲中断的失效；
5、HAL_DMA_Abort()禁止了DMA流。
```

这就是接收一次数据后不论DMA设置的Buffer有没有满（也就是NDTR不为0），DMA传输都会停止的原因：触发了串口空闲中断。前4点是从串口配置禁止，第5点是从DMA配置禁止。所以普通模式下使用空闲中断要留个心眼：空闲中断是会影响到DMA的配置的，此时DMA作用只是作为单次数据接收的缓冲区，每次接收完都会重置。
结论：普通模式下，使用HAL_UARTEx_ReceiveToIdle_DMA()，一旦进入过空闲中断就只能再一次调用HAL_UARTEx_ReceiveToIdle_DMA()来开启DMA和串口的对应配置。

#### 循环模式下

反过来说，**循环模式**下，空闲中断就和DMA的运作独立开来了。DMA配置的Memory可以看作一个环形缓冲区，里面的数据并不会因为空闲中断清空。当我们配置好后，就得到了一个会自动写入数据的环形缓冲区，接下来就是如何从里面读取数据了。

关于环形缓冲区的介绍可以看这篇文章：[ring buffer，一篇文章讲透它](https://zhuanlan.zhihu.com/p/534098236)，讲得非常通俗易懂。

实现思路
读数据的前提肯定是有新的数据写入。空闲中断会通知我们数据更新了，因此首当其冲的问题是如何获取新写入到缓冲区数据的长度。 循环模式跟普通模式不同，NDTR不会每次接收完都被重置，但传入HAL_UARTEx_RxEventCallback()的参数Size却是 （预设的缓冲区大小 - NDTR），也就是已用缓冲区大小，具体参考HAL_UART_IRQHandler()下面的几行代码：

```c
/* Check if DMA mode is enabled in UART */
    if (HAL_IS_BIT_SET(huart->Instance->CR3, USART_CR3_DMAR))
    { /* DMA mode enabled */
      uint16_t nb_remaining_rx_data = (uint16_t) __HAL_DMA_GET_COUNTER(huart->hdmarx);	//返回的值就是NDTR
	 ......
      huart->RxXferCount = nb_remaining_rx_data;
     ......
      HAL_UARTEx_RxEventCallback(huart, (huart->RxXferSize - huart->RxXferCount));
    }

```

因此普通模式下把`Size`当作写入长度是没问题的，但循环模式下就需要我们进一步计算了。

既然把循环模式的数据存放看作环形缓冲区，那`Size`不就基本等同于一个自动更新的写索引了吗？**我们维护一个自定义写索引`rear`，每次进入空闲中断都更新一次**：

```c
rear = (Size != sizeof(RxBuffer))?Size:0;		//若Size等于缓冲区长度证明写满了，写指针要回到起点
```

很容易就能得出写入数据的长度：

若Size > rear，写入数据长度 = Size - rear；
若Size <= rear，写入数据长度 = Size + (sizeof(RxBuffer) - rear)。

**使用STM32Cube FW_F4 V1.28.2 之前版本的请仔细阅读以下内容（新版本请忽略）！！！**
多谢评论区里@quote6的一同探讨和调试，我发现在25年4月发布的STM32Cube FW_F4 V1.28.2的固件代码中，数据刚好写满缓冲区的情况已经会同时触发全满中断和空闲中断，因此下述内容请根据自己使用的固件版本来作参考！(2025.8.4新增说明)

**紧接而来就有一个坑了**：看到这里大家可能会想到，把全满中断关掉，反正也用不上，开着还会进入中断回调，还可能影响到上面的运算逻辑。这个想法本身是没问题的，但实际上回看上面普通模式贴的代码就会发现一个问题：

```c
	 /* DMA mode enabled */
	   uint16_t nb_remaining_rx_data = (uint16_t) __HAL_DMA_GET_COUNTER(huart->hdmarx);
	   if ((nb_remaining_rx_data > 0U) && (nb_remaining_rx_data < huart->RxXferSize))
	   {
	        if (huart->hdmarx->Init.Mode != DMA_CIRCULAR)
	        {......}	
	        	
	        /*Call legacy weak Rx Event callback*/
   			HAL_UARTEx_RxEventCallback(huart, (huart->RxXferSize - huart->RxXferCount));
   	   }

```

调用回调函数的条件，普通模式和循环模式是一样的：**NDTR>0**，且**NDTR < 预设的BufferSize** 。循环模式下一旦NDTR变为0会立刻重置为初始预设值，也就是说如果写入的数据刚好写满缓冲区，进入HAL_UART_IRQHandler() 时是 **NDTR == 预设的BufferSize**，自然就不会调用回调函数了！

不过虽说不能关全满中断，但其实我们一般不用关心数据是哪次中断进来的，只需要每次中断时把缓冲区的数据读取出来并放到FIFO就好了。因此每次中断回调中我们都对数据处理，就不会出现Size<=rear的情况了（全满中断时rear设成0）。

***注意：下面的做法只有需要对数据分帧的才用关注，不然请忽略！！！***

但利用空闲中断处理数据的好处就是能对数据进行分帧。如果想实现，就必须在全满中断中解决单次数据刚好写满缓冲区的情况。目前想到的解决思路如下：

进入中断回调时若**Size** == **sizeof(RxBuffer)** （或 **huart->RxEventType** == **HAL_UART_RXEVENT_TC**），证明触发的是全满中断，此时需要加入一定延时（不能直接用**HAL_Delay()**，SysTick中断抢占优先级最低，在这里调用只会导致程序卡死），等待DMA传输一个单位数据的周期，然后进行如下判断：

​	若**NDTR** == **sizeof(RxBuffer)**，证明触发中断后没有再往缓冲区里写数据了（中断不会打断DMA传输），本次写入就是刚好写满缓冲区。运算和之前所述一样；

​	若**NDTR** != **sizeof(RxBuffer)**，证明数据还没写完，我们只要等待空闲中断到来再处理就行了，因此直接return即可；

进入中断回调时若**Size** != **sizeof(RxBuffer)**，证明触发的是空闲中断，和之前所述同样处理。

**ps：**
写入的数据刚好填满缓冲区，触发的是全满中断而不是空闲中断；
关于触发了全满中断后判断DMA是否仍在写入数据，目前我只能想到加延时再判断，若大家有更好的办法请不吝赐教。

代码仅供参考：

```c
#define UART_RXBUF_MAXSIZE (8)

//串口环形缓冲区结构
typedef struct
{
    uint16_t front;            				//写索引
    uint16_t rear;           				//读索引
    uint8_t buf[UART_RXBUF_MAXSIZE];      	//缓冲区
}uart_buf_t, * p_uart_buf_t;

uart_buf_t rxCirBuf = {};

//读取串口缓冲区
int get_data_fromRxBuf(uint8_t* data,uint16_t size)
{
	p_uart_buf_t p = &rxCirBuf;

	if(size == 0 || size > sizeof(p->buf))
		return -1;

	if(p->front < p->rear)
	{
		memcpy(data,p->buf + p->front,size);
		p->front += size;
	}
	else
	{
		uint16_t rest = sizeof(p->buf) - p->front;
		memcpy(data,p->buf + p->front,rest);
		memcpy(data+rest,p->buf,size - rest);
		p->front = size - rest;
	}
	return 1;
}

//接收中断回调函数
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef * huart, uint16_t Size)
{
	if(huart->Instance == USART1)
	{
		uint16_t BUFF_SIZE = sizeof(rxCirBuf.buf);
		//判断是否为全满中断
		if(Size == BUFF_SIZE)
		{
			delay_us(100);			//这里记得要用非中断延时
			uint16_t NDTR = (uint16_t) __HAL_DMA_GET_COUNTER(huart->hdmarx);
			if(NDTR != BUFF_SIZE)
				return;
		}
		//计算数据长度
		uint16_t size = (Size > rxCirBuf.rear)?\
						(Size - rxCirBuf.rear):(Size + (BUFF_SIZE - rxCirBuf.rear));
		//写索引更新
		rxCirBuf.rear = (Size != BUFF_SIZE)?Size:0;			
		
		/*	TODO:	*/		
		//测试用：发送接收到的数据
		uint8_t dSend[size];
		if(1 == get_data_fromRxBuf(dSend, size))
			HAL_UART_Transmit(&huart1, dSend, size,20);         
	}
}

```

上面代码的测试部分还包含了读取缓冲区的操作，本文不再展开。

#### 总结说明

上面所述的是如何使用循环模式下的环形缓冲区，包括应该在哪里、在什么时候处理接收的数据，并且如何获取到接收到数据的长度。每个人的应用场景不同，有可能处理的数据每帧都有包头包尾，或每帧数据是定长，或数据中就包含长度信息，因此处理时并不一定要计算size。本文只是介绍了一个实现思路，环形缓冲区也只是进行了最基本的实现，实质上还有很多可挖掘可完善的地方，比如这篇博客里提到的一种情况：处理一帧数据过久会出现的情况（@大文梅）。

### 2、DMA + IDLE中断接收 DMA缓冲区溢出触发 错误回调函数 

在实际开发当中，偶尔会因为某一小段时间内接收大量数据（此数据量大大超过了DMA缓冲区所能承受的范围），并且在HAL_UARTEx_RxEventCallback 函数中由于处理时间过长导致程序进入 错误状态，从而不能有效唤醒 DMA的情况。

**错误复现**

![](./image/QQ20250925-114453.png)

![](./image/QQ20250925-114243.png)

当我从 PC 机上像单片机发送一个数据包大小大于 20 的数据时，单片机只会返回一个 "Half full",然后再次发送数据，此时就不会再有反馈信息了。

**现象解释：**

当发送一个大于 DMA 所能承受的数据包的时候，第一次进入中断服务函数是在 “半满” 的状态下，当 
Size == MAXBUFSIZE / 2 时，进入 HAL_UARTEx_RxEventCallback 中断回调，然后执行相应逻辑（**注意：**这里进入半满的时候，DMA不会失效，因此也不用在调用HAL_UARTEx_RxEventCallback() 重新唤醒 DMA），由于在 半满 的逻辑中，调用了 打印 的函数，比较耗时，此时 DMA 是处于有效状态，DMA 就会继续进行数据搬运，此时 cpu 会进行数据的打印工作，在打印的过程中，DMA 缓冲区满了，立即出发全满中断，但是，由于 printf 耗时太长，cpu还没有执行完上一次的中断处理，此时又来了 全满 中断，cpu就不能及时处理这次中断， **接收寄存器的数据没有及时被读走**，但新的数据已经到来，旧数据被覆盖。因此就会触发 **ORE溢出中断**，从而导致cpu直接进入 **错误中断回调函数**，在HAL_UART_ErrorCallback内，用户需要进行补救措施，然后重新开启DMA

**总结：**

1、DMA 先把前 20 个字节存入 buf

2、DMA 完成传输 → `HAL_UART_RxCpltCallback()` 触发

3、但此时 UART 硬件还在继续接收剩下的 5 个字节

4、因为 DMA 没有重新配置，RDR 数据寄存器没有地方写 → **ORE 溢出错误**

5、于是 `HAL_UART_ErrorCallback()` 被触发

**补救：**

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if(huart->ErrorCode & HAL_UART_ERROR_NE ){
        /* 处理噪声错误 */
    }
    else if( huart->ErrorCode & HAL_UART_ERROR_ORE ){
        /* 进行溢出恢复措施 */
//		 __HAL_UART_CLEAR_OREFLAG(huart);       // 清溢出标志
        HAL_UART_DMAStop(huart);               // 停止 DMA
		flag[1] = 1;
        HAL_UARTEx_ReceiveToIdle_DMA(huart, buf, sizeof(buf)); // 重新启动 DMA
    }
}
```



------

## 使用DMA进行串口发送

来都来了，顺便简单讲一下`HAL_UART_Transmit_DMA()`发送数据的流程。
首先接收数据和发送数据不是同一个DMA流，数据流的方向都不一样，如果发现无法发送数据不妨先检查下是否忘记配置串口Tx的DMA流。

![img](https://i-blog.csdnimg.cn/direct/59142128ecb54ba5a1b7844bec66e3dc.png)

### 大概的工作流程

1、在HAL_UART_Transmit_DMA()里，发送数据前会自动打开中断使能和DMA流使能并清空中断标志位，配置好后令串口控制寄存器的DMAT位置1（USART_CR3_DMAT）使能DMA Mode；

2、使能后DMA开始从Memory将数据写入到TDR，而当TDR的数据全部送到移位寄存器后，TXE Flag会置1，说明TDR空了，DMA就又送入下一个数据了。 特别说明一下，整个过程TXEIE位是disabled的，因此每次TXE Flag置1都不会触发中断。另外HAL_UART_Transmit()也是一样，只有HAL_UART_Transmit_IT()会使能TXEIE来利用TXE中断发送数据。 这样大家应该可以理解这三者的区别了，包括阻塞发送和非阻塞发送也清晰了。别再写完 HAL_UART_Transmit_IT(&huart1, rxBuf, Size) 或 HAL_UART_Transmit_DMA(&huart1, rxBuf, Size) 后接一句 memset(rxBuf, 0, Size) 然后问为什么发不出数据或者少数据了；

3、当全部数据数传输完成后，NDTR计数也到0了，触发了全满中断从而进入了HAL_DMA_IRQHandler()。HAL库会自动清空中断标志位，若非循环模式还会禁止DMA中断。最后调用库内置的回调函数UART_DMATransmitCplt()；

4、UART_DMATransmitCplt()中会将DMAT置0，禁止DMA Mode；然后USART_CR1_TCIE置1使能传输完成中断。最后调用回调函数HAL_UART_TxCpltCallback()，用户可以重写该函数来实现自己的需求。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/1185ac6f3c5d4e929fa3eba8e1be5248.png)

------

## 环形缓冲区

### **1 概述**

**ring buffer**称作**[环形缓冲区](https://zhida.zhihu.com/search?content_id=206821987&content_type=Article&match_order=1&q=环形缓冲区&zd_token=eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJ6aGlkYV9zZXJ2ZXIiLCJleHAiOjE3NTg5NDM1ODYsInEiOiLnjq_lvaLnvJPlhrLljLoiLCJ6aGlkYV9zb3VyY2UiOiJlbnRpdHkiLCJjb250ZW50X2lkIjoyMDY4MjE5ODcsImNvbnRlbnRfdHlwZSI6IkFydGljbGUiLCJtYXRjaF9vcmRlciI6MSwiemRfdG9rZW4iOm51bGx9.XwtHgjyUWl_u6-W_taFv6erk-Q2oso3IICvQlIKRoHk&zhida_source=entity)**，也称作环形队列（circular queue），是一种用于表示一个固定尺寸、头尾相连的缓冲区的数据结构，适合缓存数据流。如下为**环形缓冲区（ring buffer）** 的概念示意图。



在任务间的通信、串口数据收发、log缓存、网卡处理网络数据包、音频/视频流处理中均有**环形缓冲区（ring buffer）** 的应用。在**[RT-Thread](https://zhida.zhihu.com/search?content_id=206821987&content_type=Article&match_order=1&q=RT-Thread&zd_token=eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJ6aGlkYV9zZXJ2ZXIiLCJleHAiOjE3NTg5NDM1ODYsInEiOiJSVC1UaHJlYWQiLCJ6aGlkYV9zb3VyY2UiOiJlbnRpdHkiLCJjb250ZW50X2lkIjoyMDY4MjE5ODcsImNvbnRlbnRfdHlwZSI6IkFydGljbGUiLCJtYXRjaF9vcmRlciI6MSwiemRfdG9rZW4iOm51bGx9.StfHXmMlPVoTlkdamednV9MmfZErU-0-VRua4zxRPkk&zhida_source=entity)**的`ringbuffer.c`和`ringbuffer.h`文件中，**Linux**内核文件`kfifo.h`和`kfifo.c`中也有**环形缓冲区（ring buffer）**的代码实现。

![动图封面](https://pica.zhimg.com/v2-846878d6005f23f9f15afde8fe0ae028_b.jpg)



环形缓冲区的一些使用特点如下：

- 当一个数据元素被读取出后，其余数据元素不需要移动其存储位置；
- 适合于事先明确了缓冲区的最大容量的情形。缓冲区的容量（长度）一般固定，可以用一个静态数组来充当缓冲区，无需重复申请内存；
- 如果缓冲区的大小需要经常调整，就不适合用环形缓冲区，因为在扩展缓冲区大小时，需要搬移其中的数据，这种场合使用链表更加合适；
- 因为缓冲区成头尾相连的环形，写操作可能会覆盖未及时读取的数据，有的场景允许这种情况发生，有的场景又严格限制这种情况发生。选择何种策略和具体应用场景相关。

### **2 原理**

由于计算机内存是线性地址空间，因此**环形缓冲区（ring buffer）**需要特别的算法设计才可以从逻辑上实现。

#### **2.1 一个简单例子**

先不要想**环形缓冲区（ring buffer）**的具体实现细节，来看一个简单的例子。如下是一个空间大小为 **7**的环形缓冲区，其中底部的单线箭头表示“头尾相连”形成一个环形地址空间：

![img](https://pic2.zhimg.com/v2-3c708766d94d38cf4200178999dd7599_1440w.jpg)

假定`1`被写入缓冲区中部（对于环形缓冲区来说，最初的写入位置在哪里是无关紧要的）：

![img](https://pic3.zhimg.com/v2-3a8ce87ad6a58e055b539be393b27e18_1440w.jpg)



再**写入两个**元素，分别是`2`和`3`，这两个元素被追加到`1`之后：

![img](https://pic4.zhimg.com/v2-eabe9ec4a3b9f9e0baf9f00694ca86ff_1440w.jpg)



如果**读出两个**元素，那么环形缓冲区中最老的两个元素将被读出（先进先出原则）。在本例中`1`和`2`被读出，缓冲区中剩下`3`：

![img](https://pic4.zhimg.com/v2-64d494cde67e724293a94d4e0ab7bbfd_1440w.jpg)



紧接着，向缓冲区中**写入六个**元素`4、5、6、7、8、9`，这时缓冲区会被装满：

![img](https://picx.zhimg.com/v2-7926b90699fc686536533549dea6f1e5_1440w.jpg)



如果缓冲区是满的，又要写入新的数据，这时有两种策略：一种是覆盖掉最老的数据，也就是将老数据丢掉；另一种是返回错误码或者抛出异常。来看策略一，例如，这时**写入两个**元素`A`和`B`，就会覆盖掉`3`和`4`：

![img](https://pic1.zhimg.com/v2-b16ea940dd23d91c3438b608afd465d8_1440w.jpg)



再看，这时如果**读出两个**元素，就不是`3`和`4`而是`5`和`6`（`5`和`6`这时最老），`3`和`4`已经被`A`和`B`覆盖掉。

![img](https://pic3.zhimg.com/v2-cccac7c12e03b751d7e5f34edda10b56_1440w.jpg)



通过这个简单的例子，可以总结出要实现**环形缓冲区（ring buffer）**需要注意到几个问题点：

1. 在缓冲区满的时候写数据，有两种策略可以使用：第一覆盖掉老数据；第二抛出异常；
2. 读数据时，一定要读出缓冲区中最老的数据（缓冲区中数据满足FIFO特性）；
3. 怎样来判断缓冲区是满的；
4. 如何实现一个线性地址空间的循环读写。

#### **2.2 具体怎样做**

一般的，对一个环形缓冲区进行读写操作，最少需要**4**个信息：

- 在内存中的实际**开始位置**（例如：一片内存的头指针，数组的第一个元素指针）；
- 在内存中的实际**结束位置**（也可以是缓冲区实际空间大小，结合开始位置，可以算出结束位置）；
- 在缓冲区中进行写操作时的**写索引**值；
- 在缓冲区中进行读操作时的**读索引**值。

**缓冲区开始位置**和**缓冲区结束位置（或空间大小）** 实际上定义了环形缓冲区的实际逻辑空间和大小。**读索引**和**写索引**标记了缓冲区进行读操作和写操作时的具体位置。如下图所示，为环形缓冲区的典型读写过程：

1. 当环形缓冲区为空时，读索引和写索引指向相同的位置（因为是环形缓冲区，可以出现在任何位置）；
2. 当向缓冲区写入一个元素时，元素`A`被写入**写索引**当前所指向位置，然后写索引加1，指向下一个位置；
3. 当再写如一个元素`B`时，元素`B`继续被写入**写索引**当前所指向位置，然后写索引加1，指向下一个位置；
4. 当接着写入`C`、`D`、`E`、`F`、`G`五个元素后，缓冲区就满了，这时**写索引**和**写索引**指向同一个位置(和缓冲区为空时一样)；
5. 当从缓冲区中读出一个元素`A`时，**读索引**当前所在位置的元素被读出，然后**读索引**加1，指向下一个位置；
6. 继续读出元素`B`时，还是**读索引**当前所在位置的元素被读出，然后**读索引**加1，指向下一个位置。

![img](https://pic1.zhimg.com/v2-67987366b04587ac940c666c4f83a860_1440w.jpg)

下面来探讨具体怎样做？在**2.1节**中提出了四个问题点，本节将会对这四个问题点进行回答，在回答完这四个问题点后，对于如何实现**环形缓冲区（ring buffer）**的脉络应该会清晰起来。

#### **2.2.1 在缓冲区满的时候写数据，有两种策略可以使用**

缓冲区变满在**环形缓冲区（ring buffer）**中会实际发生，一般会有两种处理策略，第一覆盖掉老数据；第二抛出“异常”。这两种策略该如何选择要结合具体的应用场景。如音/视频流中，丢掉一些数据不要紧，可以选择第一种策略；在任务间通信的时候，要严格保证数据正确传输，这个时候就要选择第二种策略。

#### **2.2.2 读数据时，一定要读出缓冲区中最老的数据**

**环形缓冲区（ring buffer）**也是`FIFO`类型的数据结构，需要满足先进先出的原则。**写**就相当于进，**读**就相当于出。所以读数据时，一定要保证读最老的数据。一般的情况下不会有问题，但有一种场景需要小心。如下图所示环形缓冲区的大小为 **七**，缓冲区中已经存储了`7`，`8`，`9`，`3`，`4`五个元素。

![img](https://pica.zhimg.com/v2-1af2a1bf448a43cbbcf70b9afdf4a482_1440w.jpg)

如果再向缓冲区中写入三个元素`A`，`B`，`C`，因为剩余空间为**2**了，所以要想写入这三个元素肯定会覆盖掉一个元素。此时，读/写索引变为如下图所示。读索引不再停留在元素`7`处，而是在元素`8`处，因为元素`7`已经被覆盖掉，而元素`8`变为最老的元素。这个例子说明，当缓冲区是满的时候，继续写入元素（覆盖），除了**写索引**要变，**读索引**也要跟着变，保证**读索引**一定是指向缓冲区中最老的元素。

![img](https://pica.zhimg.com/v2-476b7cc48e208abcd634c9c5222bed38_1440w.jpg)

#### **2.2.3 怎样来判断缓冲区是满的**

如下图，缓冲区是满、或是空，都有可能出现读索引与写索引指向同一位置：

![img](https://pic2.zhimg.com/v2-d1c9c30ea0e1338d15a21005ae1b0ebd_1440w.jpg)

判断缓冲区是满还是空，在**环形缓冲区（ring buffer）**中是一个重点问题，在维基百科([http://en.wikipedia.org/wiki/Circular_buffer](https://link.zhihu.com/?target=http%3A//en.wikipedia.org/wiki/Circular_buffer))中，讲解了五种判断方法，感兴趣可以看一下。在平衡各方优缺点后，本节重点讲解 **[镜像指示位](https://zhida.zhihu.com/search?content_id=206821987&content_type=Article&match_order=1&q=镜像指示位&zd_token=eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJ6aGlkYV9zZXJ2ZXIiLCJleHAiOjE3NTg5NDM1ODYsInEiOiLplZzlg4_mjIfnpLrkvY0iLCJ6aGlkYV9zb3VyY2UiOiJlbnRpdHkiLCJjb250ZW50X2lkIjoyMDY4MjE5ODcsImNvbnRlbnRfdHlwZSI6IkFydGljbGUiLCJtYXRjaF9vcmRlciI6MSwiemRfdG9rZW4iOm51bGx9.PyNR70sIGm5_hY-_KX3S1ksr2VM1M4fDrrN97nhY35M&zhida_source=entity)**方法，在`linux`和`RT-Thread`实现的环形缓冲区中，也都是用的该策略（或者说是该策略的扩展）。

> **镜像指示位**：缓冲区的长度如果是n，逻辑地址空间则为0至n-1；那么，规定n至2n-1为镜像逻辑地址空间。本策略规定读写指针的地址空间为0至2n-1，其中低半部分对应于常规的逻辑地址空间，高半部分对应于镜像逻辑地址空间。当指针值大于等于2n时，使其折返（wrapped）到ptr-2n。使用一位表示写指针或读指针是否进入了虚拟的镜像存储区：置位表示进入，不置位表示没进入还在基本存储区。

![img](https://pic4.zhimg.com/v2-22cba7a9b8ff0746184d752a63621981_1440w.jpg)

在读写指针的值相同情况下，如果二者的指示位相同，说明缓冲区为空；如果二者的指示位不同，说明缓冲区为满。这种方法优点是测试缓冲区满/空很简单；不需要做取余数操作；读写线程可以分别设计专用算法策略，能实现精致的并发控制。 缺点是读写指针各需要额外的一位作为指示位。
如果缓冲区长度是2的幂，则本方法可以省略镜像指示位。如果读写指针的值相等，则缓冲区为空；如果读写指针相差n，则缓冲区为满，这可以用条件表达式（写指针 == (读指针 异或 缓冲区长度)）来判断。—— 维基百科

上面是维基百科中对**镜像指示位**的整体描述，但是单凭上面这一个描述，去理解**镜像指示位**方法还是有一定困难，下面来进行一些讨论。

上面描述中提到了**读/写指针**的概念，注意这个读/写指针和上文提到的**读索引**和**写索引**不是一回事。读写指针的范围是，[0，2𝑛−1]，而**读索引**和**写索引**的范围是，[0，𝑛−1]，其必须和缓冲区的实际逻辑空间一致。但是**读/写指针**和**读索引**和**写索引**有一个转换关系：

读索引读指针缓冲区长度读索引=读指针%缓冲区长度写索引写指针缓冲区长度写索引=写指针%缓冲区长度

其中%号，是求余运算符。但是如果缓冲区长度是2的幂，那么求余运算可以等价的转换为如下的**按位与**运算：

读索引读指针缓冲区长度读索引=读指针&(缓冲区长度−1)写索引写指针缓冲区长度写索引=写指针&(缓冲区长度−1)

按位与的运算效率要比求余运算高的多，在`linux`内核中将缓冲区长度扩展为2的幂长度随处可见，都是为了用按位与操作代替求余操作。为了判断缓冲区是否为**空或者满**，镜像指示位策略引入了两个布尔变量（指示位），来分别标记读指针或写指针是否进入了镜像区间，[𝑛，2𝑛−1]，在读写指针的值相同情况下，如果二者的指示位相同，说明缓冲区**为空**；如果二者的指示位不同，说明缓冲区**为满**。但如果缓冲区的长度是2的幂，则可以省略镜像指示位。如果读写指针的值相等，则缓冲区为空；如果读写指针相差n，则缓冲区为满。

#### **2.2.4 如何实现一个线性地址空间的循环读写**

理解了**2.2.3节**的描述，再来理解用一个线性地址空间来实现循环读写就比较容易。如一个环形缓冲区的长度为**七**，则其读写索引的区间为，[0，6]。当写索引的值为`6`，再向缓冲区中写入一个元素时，写索引应该要回到缓冲区的起始索引位置`0`，读索引在碰到这种情况也是类似处理。总结为一句话就是，当写索引或读索引已经到了环形缓冲区的结束位置时，进行下一步操作时，其应该要回到环形缓冲区的开始位置。

### **3 实现**

对于环形缓冲区的代码实现，本文会分析**RT-Thread**的`ringbuffer.c`和`ringbuffer.h`文件，**Linux**内核中的`kfifo.h`和`kfifo.c`文件。

#### **3.1 RT-Thread中实现的ring buffer**

下面分析**RT-Thread**的**ring buffer**实现，主要会讨论**环形缓冲区结构体**、**缓冲区初始化操作**、**写操作**、**读操作**、**判断缓冲区是否为空或满**。

##### **环形缓冲区结构体**

`RT-Thread`中定义了结构体`rt_ringbuffer`，其中`buffer_ptr`、`buffer_size`、`read_index`、`write_index`和**2.2节**中介绍的**4**个信息是完全对应的。为了判断缓冲区是空还是满，还定义了两个布尔型变量`read_mirror`和`write_mirror`，其是通过位域的定义方式来实现。

```c
struct rt_ringbuffer
{
    rt_uint8_t *buffer_ptr;
    rt_uint16_t read_mirror : 1;
    rt_uint16_t read_index : 15;
    rt_uint16_t write_mirror : 1;
    rt_uint16_t write_index : 15;
    rt_int16_t buffer_size;
};
```

##### **缓冲区初始化操作**

初始化操作`rt_ringbuffer_init`很容易理解，就是将申请好的内存地址赋值给环形缓冲区，缓冲区实际逻辑大小也传入进去。`read_index`、`write_index`、`read_mirror`和`write_mirror`全部初始化为零。

```c
void rt_ringbuffer_init(struct rt_ringbuffer *rb,
                        rt_uint8_t           *pool,
                        rt_int16_t            size)
{
    RT_ASSERT(rb != RT_NULL);
    RT_ASSERT(size > 0);

    /* initialize read and write index */
    rb->read_mirror = rb->read_index = 0;
    rb->write_mirror = rb->write_index = 0;

    /* set buffer pool and size */
    rb->buffer_ptr = pool;
    rb->buffer_size = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);
}
```

##### **写操作和读操作**

写操作有两个接口`rt_ringbuffer_put`和`rt_ringbuffer_put_force`，当缓冲区满的时候，前一个不会写入，后一个会强制写入（覆盖）；读操作有一个接口`rt_ringbuffer_get`。

这里先说明一下，**RT-Thread**的**ring buffer**实现虽然借鉴了上一章讲的**镜像指示位**策略，但其并没有使用读写指针，而是直接用的**写索引**和**读索引**，也就是说结构体中的`read_index`和`write_index`就是**写索引**和**读索引**，无需进行转换，直接可以用来操作缓冲区。这一点和`linux`的实现方式不同，在下面的`linux`章节中会看到。但`read_mirror`和`write_mirror`是和**镜像指示位**策略中讲的一样，用来标记是否进入了镜像区间。

先来看`rt_ringbuffer_put`的实现，该函数的返回值是实际写入大小，就是如果传入的`length`大于缓冲区的剩余空间，则`length`只有部分会被写入缓冲区。通过条件`if (rb->buffer_size - rb->write_index > length)`将写操作分成了如下两种情形：

![img](https://pica.zhimg.com/v2-63929e833dfa2be0a5c0359458efe69e_1440w.jpg)

当`if (rb->buffer_size - rb->write_index > length)`为**真**时，其对应的是**情形一**。就是说从**写索引**到缓冲区结束位置这一段空间能容纳全部所写入数据。在图片情形一中，开始缓冲区中有两个元素`1`、`2`，接着继续写入`A`、`B`、`C`三个元素。可以看出从**写索引**到缓冲区结束位置还可以容纳**4**个元素，所以`A`、`B`、`C`三个元素可以一次性写入缓冲区，**写索引**无需回环。对应的代码就是`rt_memcpy(&rb->buffer_ptr[rb->write_index], ptr, length);`。

当`if (rb->buffer_size - rb->write_index > length)`为**假**时，其对应的是**情形二**。就是说从**写索引**到缓冲区结束位置这一段空间无法全部容纳所写入数据，**写索引**需要回环到缓冲区开头，写入剩下的数据。在图片情形二中，开始缓冲区中有两个元素`1`、`2`，接着继续写入`A`、`B`、`C`三个元素。可以看出从**写索引**到缓冲区结束位置还可以容纳**2**个元素，也就是说`A`、`B`两个元素可以写入从**写索引**到缓冲区结束位置这一段，而`C`只能回环到缓冲区的开头位置。对应代码就是`rt_memcpy(&rb->buffer_ptr[rb->write_index],&ptr[0],rb->buffer_size - rb->write_index);rt_memcpy(&rb->buffer_ptr[0],&ptr[rb->buffer_size - rb->write_index],length - (rb->buffer_size - rb->write_index));`。因为写索引已经回环了，所以要将`write_mirror`做一下取反操作：`rb->write_mirror = ~rb->write_mirror;`。

写操作接口`rt_ringbuffer_put_force`和上面介绍的基本一样，其实就是多了当传入的`length`大于缓冲区的剩余空间时，会将已有的元素覆盖掉。如果发生了元素覆盖，那缓冲区一定会变满，read_index和write_index回相等，对应语句`if (length > space_length) rb->read_index = rb->write_index;`（也即2.1节中讨论的问题2，读索引要指向最老元素）。因为会操作`read_index`元素，也要考虑其是否发生了回环，发生了回环后`read_mirror`需要取反，对应语句`rb->read_mirror = ~rb->read_mirror;`。图中对应的**情形三**描述了该过程，开始缓冲区中有两个元素`1`、`2`，接着继续写入`A`、`B`、`C`、`D`、`E`、`F`六个元素。此时元素`1`被覆盖掉，**写索引**和**读索引**都指向元素`2`。

读接口`rt_ringbuffer_get`和写接口的操作逻辑基本一致，也是通过条件`if (rb->buffer_size - rb->write_index > length)`将读操作分成了两种情形，过程和写操作接口`rt_ringbuffer_put`没有差异。

```c
/**
 * @brief Put a block of data into the ring buffer. If the capacity of ring buffer is insufficient, it will discard out-of-range data.
 *
 * @param rb            A pointer to the ring buffer object.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of data in bytes.
 *
 * @return Return the data size we put into the ring buffer.
 */
rt_size_t rt_ringbuffer_put(struct rt_ringbuffer *rb,
                            const rt_uint8_t     *ptr,
                            rt_uint16_t           length)
{
    rt_uint16_t size;

    RT_ASSERT(rb != RT_NULL);

    /* whether has enough space */
    size = rt_ringbuffer_space_len(rb);

    /* no space */
    if (size == 0)
        return 0;

    /* drop some data */
    if (size < length)
        length = size;

    if (rb->buffer_size - rb->write_index > length)
    {
        /* read_index - write_index = empty space */
        rt_memcpy(&rb->buffer_ptr[rb->write_index], ptr, length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->write_index += length;
        return length;
    }

    rt_memcpy(&rb->buffer_ptr[rb->write_index],
           &ptr[0],
           rb->buffer_size - rb->write_index);
    rt_memcpy(&rb->buffer_ptr[0],
           &ptr[rb->buffer_size - rb->write_index],
           length - (rb->buffer_size - rb->write_index));

    /* we are going into the other side of the mirror */
    rb->write_mirror = ~rb->write_mirror;
    rb->write_index = length - (rb->buffer_size - rb->write_index);

    return length;
}
RTM_EXPORT(rt_ringbuffer_put);

/**
 * @brief Put a block of data into the ring buffer. If the capacity of ring buffer is insufficient, it will overwrite the existing data in the ring buffer.
 *
 * @param rb            A pointer to the ring buffer object.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of data in bytes.
 *
 * @return Return the data size we put into the ring buffer.
 */
rt_size_t rt_ringbuffer_put_force(struct rt_ringbuffer *rb,
                            const rt_uint8_t     *ptr,
                            rt_uint16_t           length)
{
    rt_uint16_t space_length;

    RT_ASSERT(rb != RT_NULL);

    space_length = rt_ringbuffer_space_len(rb);

    if (length > rb->buffer_size)
    {
        ptr = &ptr[length - rb->buffer_size];
        length = rb->buffer_size;
    }

    if (rb->buffer_size - rb->write_index > length)
    {
        /* read_index - write_index = empty space */
        rt_memcpy(&rb->buffer_ptr[rb->write_index], ptr, length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->write_index += length;

        if (length > space_length)
            rb->read_index = rb->write_index;

        return length;
    }

    rt_memcpy(&rb->buffer_ptr[rb->write_index],
           &ptr[0],
           rb->buffer_size - rb->write_index);
    rt_memcpy(&rb->buffer_ptr[0],
           &ptr[rb->buffer_size - rb->write_index],
           length - (rb->buffer_size - rb->write_index));

    /* we are going into the other side of the mirror */
    rb->write_mirror = ~rb->write_mirror;
    rb->write_index = length - (rb->buffer_size - rb->write_index);

    if (length > space_length)
    {
        if (rb->write_index <= rb->read_index)
            rb->read_mirror = ~rb->read_mirror;
        rb->read_index = rb->write_index;
    }

    return length;
}
RTM_EXPORT(rt_ringbuffer_put_force);

/**
 * @brief Get data from the ring buffer.
 *
 * @param rb            A pointer to the ring buffer.
 * @param ptr           A pointer to the data buffer.
 * @param length        The size of the data we want to read from the ring buffer.
 *
 * @return Return the data size we read from the ring buffer.
 */
rt_size_t rt_ringbuffer_get(struct rt_ringbuffer *rb,
                            rt_uint8_t           *ptr,
                            rt_uint16_t           length)
{
    rt_size_t size;

    RT_ASSERT(rb != RT_NULL);

    /* whether has enough data  */
    size = rt_ringbuffer_data_len(rb);

    /* no data */
    if (size == 0)
        return 0;

    /* less data */
    if (size < length)
        length = size;

    if (rb->buffer_size - rb->read_index > length)
    {
        /* copy all of data */
        rt_memcpy(ptr, &rb->buffer_ptr[rb->read_index], length);
        /* this should not cause overflow because there is enough space for
         * length of data in current mirror */
        rb->read_index += length;
        return length;
    }

    rt_memcpy(&ptr[0],
           &rb->buffer_ptr[rb->read_index],
           rb->buffer_size - rb->read_index);
    rt_memcpy(&ptr[rb->buffer_size - rb->read_index],
           &rb->buffer_ptr[0],
           length - (rb->buffer_size - rb->read_index));

    /* we are going into the other side of the mirror */
    rb->read_mirror = ~rb->read_mirror;
    rb->read_index = length - (rb->buffer_size - rb->read_index);

    return length;
}
RTM_EXPORT(rt_ringbuffer_get);
```

##### **判断缓冲区是否为空或满**

判断缓冲区是否为空或满，通过函数`rt_ringbuffer_status`来实现。其逻辑就是在**2.2.3节**中介绍的：在读写指针的值相同情况下，如果二者的指示位相同，说明缓冲区**为空**；如果二者的指示位不同，说明缓冲区**为满**。注意这里的**读写指针**已经在读写(`rt_ringbuffer_get`和`rt_ringbuffer_put`)过程中转换为了**读写索引**。

```c
rt_inline enum rt_ringbuffer_state rt_ringbuffer_status(struct rt_ringbuffer *rb)
{
    if (rb->read_index == rb->write_index)
    {
        if (rb->read_mirror == rb->write_mirror)
            return RT_RINGBUFFER_EMPTY;
        else
            return RT_RINGBUFFER_FULL;
    }
    return RT_RINGBUFFER_HALFFULL;
}
```

##### **小结**

- 在多线程中，对同一个环形缓冲区进行读写操作时，需要加上锁，不然存在访问不安全问题；
- 当只有一个读线程和一个写线程时，用`rt_ringbuffer_put`和`rt_ringbuffer_get`进行读写操作缓冲区是线程安全的，无需加锁；但是`rt_ringbuffer_put_force`不行，因为其可能对**读写索引**都进行操作的场景，这个时候再进行`rt_ringbuffer_get`读操作，就是不安全访问；
- **读写指针**已经在读写(`rt_ringbuffer_get`和`rt_ringbuffer_put`)过程中转换为了**读写索引**。所以`read_index`(读索引)和`write_index`(写索引)可以直接用来操作缓冲区，无需转换；
- `read_index`和`write_index`的大小区间为[0,𝑛−1]，𝑛为缓冲区大小；
- **RT-Thread**的环形缓冲区不需要`buffer`大小为**2的幂**。

#### **3.2 Linux中实现的ring buffer**

在linux内核中，kfifo就是**ring buffer**的经典实现方式，本文将介绍`linux 2.6`版本中的**ring buffer**实现方式，主要介绍**缓冲区结构体**、**缓冲区初始化**、**读操作**、**写操作**、**判断缓冲区是否为空或满**。

##### **缓冲区结构体**

`kfifo`的**ring buffer**结构体定义如下，其中`buffer`、`size`、`in`、`out`和**2.2节**中介绍的环形缓冲区**4**个信息是一一对应的。但其中`in`、`out`分别是**写指针**和**读指针**，而不是**写索引**和**读索引**。它们之间的转换关系就是**2.2.3节**介绍的转化公式。参数`lock`是自旋锁，在多进程/线程对同一个环形缓冲区进行读写操作时，需要进行锁保护。和`RT-Thread`对比，可以看到其并没有读写的镜像指示位，判断缓冲区是否为空或满呢？在下一节会进行分析。

```c
struct kfifo {
 unsigned char *buffer; /* the buffer holding the data */
 unsigned int size; /* the size of the allocated buffer */
 unsigned int in; /* data is added at offset (in % size) */
 unsigned int out; /* data is extracted from off. (out % size) */
 spinlock_t *lock; /* protects concurrent modifications */
};
```

##### **缓冲区初始化**

在`kfifo`的初始化`kfifo_init`中可以看出，其会对所传入的`size`大小进行扩展，使其满足`size`为**2的幂**。这样就可以使用**2.2.3节**中介绍的性质，**如果缓冲区的长度是2的幂，则可以省略镜像指示位。如果读写指针的值相等，则缓冲区为空；如果读写指针相差n(缓冲区大小)，则缓冲区为满**。所以在传入`buffer`的`size`大小时，最好开始就将其确定为**2的幂**。

```c
struct kfifo *kfifo_init(unsigned char *buffer, unsigned int size,
    int gfp_mask, spinlock_t *lock)
{
 struct kfifo *fifo;

 /* size must be a power of 2 */
 BUG_ON(size & (size - 1));

 fifo = kmalloc(sizeof(struct kfifo), gfp_mask);
 if (!fifo)
  return ERR_PTR(-ENOMEM);

 fifo->buffer = buffer;
 fifo->size = size;
 fifo->in = fifo->out = 0;
 fifo->lock = lock;

 return fifo;
}
```

##### **读操作和写操作**

可以看到`kfifo`对读操作和写操作的实现非常简洁。在进行读操作和写操作时，其充分利用了无符号整型的性质。在`__kfifo_put`(写操作)和`__kfifo_get`(读操作)时，in(写指针)和out(读指针)都是正向增加的，当达到最大值时，产生溢出，使得从0开始，进行循环使用。**in(写指针)** 和**out(读指针)** 会恒定的保持如下关系：

读指针缓冲区已存储数据长度写指针读指针+缓冲区已存储数据长度=写指针

其中读指针是`out`，写指针是`in`。**out(读指针)** 永远不会超过**in(写指针)** 的大小，最多两者相等，相等就是缓冲区为空的时候。再结合在**2.2.3节**中介绍的转换关系：

读索引读指针缓冲区长度读索引=读指针&(缓冲区长度−1)写索引写指针缓冲区长度写索引=写指针&(缓冲区长度−1)

就可以对环形缓冲区进行读写操作了，下面来看源码。

先看`__kfifo_put`的源码。`len = min(len, fifo->size - fifo->in + fifo->out);`中表达的意思就是实际写入的长度一定要小于缓冲区的可用空间大小，防止发生覆盖已有元素的场景。来看这一句`l = min(len, fifo->size - (fifo->in & (fifo->size - 1)));`，其中`(fifo->in & (fifo->size - 1))`就是将**in(写指针)** 转换为**写索引**，整体表达的意思是从**写索引**到缓冲区结束位置这一段所能写入数据的大小，这一段写入操作的代码为`memcpy(fifo->buffer + (fifo->in & (fifo->size - 1)), buffer, l);`。如果这一段还不够，需要折返到缓冲区的开始位置，将剩下的部分写入到缓冲区中，其代码为`memcpy(fifo->buffer, buffer + l, len - l);`。而且因为取的是较小值𝑙𝑒𝑛>=𝑙(因为𝑙取的是较小值))，当𝑙𝑒𝑛=𝑙就说明第一段已经可以容纳所写入大小，缓冲区无需折返，第二个`memcpy`拷贝了零个字节，相当于什么也没有发生。

再看`__kfifo_get`的源码。其思路基本和`__kfifo_put`一致，了解了上面的转换关系，就比较好理解。

```text
/*
 * __kfifo_put - puts some data into the FIFO, no locking version
 * @fifo: the fifo to be used.
 * @buffer: the data to be added.
 * @len: the length of the data to be added.
 *
 * This function copies at most 'len' bytes from the 'buffer' into
 * the FIFO depending on the free space, and returns the number of
 * bytes copied.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
unsigned int __kfifo_put(struct kfifo *fifo,
    unsigned char *buffer, unsigned int len)
{
 unsigned int l;

 len = min(len, fifo->size - fifo->in + fifo->out);

 /* first put the data starting from fifo->in to buffer end */
 l = min(len, fifo->size - (fifo->in & (fifo->size - 1)));
 memcpy(fifo->buffer + (fifo->in & (fifo->size - 1)), buffer, l);

 /* then put the rest (if any) at the beginning of the buffer */
 memcpy(fifo->buffer, buffer + l, len - l);

 fifo->in += len;

 return len;
}
EXPORT_SYMBOL(__kfifo_put);

unsigned int __kfifo_get(struct kfifo *fifo,
    unsigned char *buffer, unsigned int len)
{
 unsigned int l;

 len = min(len, fifo->in - fifo->out);

 /* first get the data from fifo->out until the end of the buffer */
 l = min(len, fifo->size - (fifo->out & (fifo->size - 1)));
 memcpy(buffer, fifo->buffer + (fifo->out & (fifo->size - 1)), l);

 /* then get the rest (if any) from the beginning of the buffer */
 memcpy(buffer + l, fifo->buffer, len - l);

 fifo->out += len;

 return len;
}
EXPORT_SYMBOL(__kfifo_get);
```

##### **判断缓冲区是否为空或满**

`kfifo`中没有专门的函数判断缓冲区是否为空或满，但可以通过`__kfifo_len`函数获取**缓冲区已存储数据长度**。如果其值等于零就说明缓冲区为空，如果其值等于缓冲区大小，就说明缓冲区满。

```text
static inline unsigned int __kfifo_len(struct kfifo *fifo)
{
 return fifo->in - fifo->out;
}
```

##### **小结**

- `linux`中环形缓冲区（ring buffer）的实现，其实是对**2.2.3节**介绍的**镜像指示位**策略的扩展，读指针和写指针区间范围不再局限在镜像区间[0,2𝑛−1]，而是整个`unsigned int`大小的空间，对于32位机器，读指针和写指针的区间范围是，[0，232−1]；
- 进行扩展后，还能维持如下的关系，是因为缓冲区大小`n`会被扩展为**2的幂**，那么232肯定是`n`扩展后的整数倍，所以还是能够满足如下关系；

读索引读指针缓冲区长度读索引=读指针&(缓冲区长度−1)写索引写指针缓冲区长度写索引=写指针&(缓冲区长度−1)

- 读索引和写索引的区间范围仍然是[0,𝑛−1]；
- 在多进程/线程中，对同一个环形缓冲区进行读写操作时，需要加上锁，不然存在访问不安全问题；
- 当只有一个读进程/线程和一个写进程/线程时，无需加锁，也能保证访问安全。

#### **3.3 Linux最新版本中的ring buffer(5.17+)**

在`linux`最新版本(5.17+)的`kfifo.c`([https://github.com/torvalds/linux/blob/master/lib/kfifo.c](https://link.zhihu.com/?target=https%3A//github.com/torvalds/linux/blob/master/lib/kfifo.c))和`kfifo.h`([https://github.com/torvalds/linux/blob/master/include/linux/kfifo.h](https://link.zhihu.com/?target=https%3A//github.com/torvalds/linux/blob/master/include/linux/kfifo.h))中，其源码实现已经和`linux 2.6`版本有很大的不同，但是最新版本的**ring buffer核心思想**和`linux 2.6`版本并没有不同。

如下是最新`kfifo.h`文件中定义的ring buffer结构体，其中`in`、`out`依然是**写指针**和**读指针**，`mask`是缓冲区大小减1（做&操作，更方便的将读写指针转换为**读写索引**），`esize`缓冲区单个存储元素的字节大小(在`linux 2.6`版本中，一个元素就是一个字节大小，最新版本将其进行了扩展)，`data`缓冲区的逻辑起始地址（指针类型不再是字节）。其它的**初始化接口**、**读接口**、**写接口**、**判断缓冲区是否为空或满接口**逻辑和**3.2节**介绍的大致差不多，可以对照源码看一下。

```text
struct __kfifo {
 unsigned int in;
 unsigned int out;
 unsigned int mask;
 unsigned int esize;
 void  *data;
};
```

### **4 总结**

- 环形缓冲区（ring buffer）适合于事先明确了缓冲区的最大容量的情形。缓冲区的容量（长度）一般固定，可以用一个静态数组来充当缓冲区，无需重复申请内存；
- 如果缓冲区的大小需要经常调整，就不适合用环形缓存区，因为在扩展缓冲区大小时，需要搬移其中的数据，这种场合使用链表更加合适；
- 因为缓冲区成头尾相连的环形，写操作可能会覆盖未及时读取的数据，有的场景允许这种情况发生，有的场景又严格限制这种情况发生。选择何种策略和具体应用场景相关；
- 环形缓冲区（ring buffer）特别适合于通信双方循环发送数据的场景；
- 镜像指示位是一种高效判断缓冲区是否为空或满的策略，在**RT-Thread**和**linux**中都使用了该策略（或者是该策略的扩展），其能够保证在只有一个读线程（或进程）和一个写线程（或进程）中无需锁也能做到线程安全；
- 注意区分写指针和写索引，读指针和读索引，最终对缓冲区进行操作还是需要写索引和读索引；
- 如果自己嵌入式项目中需要使用环形缓冲区（ring buffer），可以借鉴`linux 2.6`版本的`kfifo`实现，很容易改写，而且非常高效。

------

## HAL库 UART在循环DMA模式下接收大量不定字长数据并进行乒乓缓存

一个严谨的STM32串口[DMA](https://so.csdn.net/so/search?q=DMA&spm=1001.2101.3001.7020)发送&接收（1.5Mbps波特率）机制STM32 HAL 库实现乒乓缓存加空闲中断的串口 DMA 收发机制，轻松跑上 2M 波特率

DMA在循环模式下工作时，如果在大规模传输数据时仍旧空闲中断（或传输完成中断）会有风险，因为当DMA传输数据完成，CPU介入开始拷贝DMA通道缓冲区数据时，如果此时UART继续有数据进来，DMA继续搬运数据到缓冲区，就有可能将数据覆盖，因为DMA数据搬运是不受CPU控制的，即使你关闭了CPU中断。

因此严谨的做法需要建立双buffer，CPU和DMA各自使用一块内存交替访问，即**乒乓缓存**，处理流程为：

1. DMA先将数据搬运到buf1，搬运完成通知CPU来拷贝buf1数据
2. DMA将数据搬运到buf2，与CPU拷贝buf1数据不会冲突
3. buf2数据搬运完成，通知CPU来拷贝buf2数据
4. DMA继续开始拷贝新数据

STM32大多数型号不提供现成的双缓存机制，但提供“半满中断”，即数据搬运到buf大小的一半时，可以产生一个中断信号。基于这个机制，我们可以实现双缓存功能，只需将buf空间开辟大一点即可。

1. DMA将数据搬运完成buf的前一半时，触发“半满中断”事件，Callback中通知CPU来拷贝buf前半部分数据
2. DMA继续将数据搬运到buf的后半部分，与CPU拷贝buf前半部数据不会冲突
3. buf后半部分数据搬运完成，触发“溢满中断”，Callback通知CPU来拷贝buf后半部分数据
4. DMA循环拷贝新数据

基于上述描述机制，DMA方式接收串口数据，有三种中断场景需要CPU去将buf数据拷贝到final中，分别是：

- DMA通道buf溢满（传输完成）场景，触发满溢中断（`HAL_UARTEx_RxEventCallback`）

- DMA通道buf半满场景，触发半满中断（`HAL_UART_RxHalfCpltCallback`）

- 串口空闲中断场景，触发空闲中断（

  ```
  UART_FLAG_IDLE
  ```

  ）

  ![请添加图片描述](https://i-blog.csdnimg.cn/img_convert/7ff87aba87ceea92e227dcc1b975a498.png)

也就是说，代码总共需要考虑以下几种情况：



对于情况1：在空闲中断中拷贝全部数据



1. 数据量未达到半满，触发**空闲中断**
2. 数据量达到半满，未达到满溢，先触发**半满中断**，后触发**空闲中断**
3. 数据量刚好达到满溢，先触发**半满中断**，后触发**满溢中断**
4. 数据量大于缓冲区长度，DMA循环覆盖溢出的字节

对于情况2：在半满中断中通知CPU拷贝一半的数据，DMA继续接收剩下的数据，最后在空闲中断中拷贝剩下的数据

对于情况3：在半满中断中通知CPU拷贝一半的数据，DMA继续接收剩下的数据，最后在满溢中断中拷贝剩下的一半数据

对于情况4：综合处理

不少教程会单独在三个中断回调函数中进行数据处理，可以发现代码量还是挺大的。尤其是这么写代码存在一个比较麻烦的逻辑：当DMA接收的数据量大于缓冲区大小`RX_BUFFER_SIZE`时，由于DMA工作在循环模式，那么溢出的数据会被DMA重新放到缓冲区的开始部分，从而覆盖原有的数据。要处理这部分数据势必要引入比较复杂的判断机制，还要实时更新队首和队尾的指针，导致整个程序变得比较复杂。

好在HAL库除了普通的`HAL_UART_Receive_DMA()`和`HAL_UART_RxCpltCallback()`外，HAL库还提供了`HAL_UARTEx_RxEventCallback`回调。

该回调函数会在“advanced reception service”事件发生后触发，这里的所谓高级接收服务就包括之前需要分开判断的半满中断、满溢中断和空闲中断。这三个中断触发后都会回调`HAL_UARTEx_RxEventCallback()`函数。因此在拷贝数据时，无需再单独进行中断回调类型的判断。由于DMA工作不依赖CPU，因此在该函数内要做的就是将缓冲区内的数据拷贝至目标地址。形参`Size`表示当前缓冲区内可用数据长度。

##### 一个缓冲区实现的乒乓缓存

乒乓缓存是指一个缓存写入数据时，设备从另一个缓存读取数据进行处理；数据写入完成后，两边交换缓存，再分别写入和读取数据。这样给设备留足了处理数据的时间，避免缓冲区中旧数据还没读取完又被新数据覆盖掉的情况。但是出现了一个小问题，就是 STM32 大部分型号的串口 DMA 只有一个缓冲区，要怎么实现乒乓缓存呢？

没错，半满中断。现在，一个缓冲区能拆成两个来用了。

![img](https://i-blog.csdnimg.cn/blog_migrate/091ecd09332d6cc8012b0c912187031d.png)

看这图我们再来理解一下上面提到的三个中断：接受缓冲区的前半段填满后触发半满中断，后半段填满后触发全满中断；而这两个中断都没有触发，但是数据包已经结束且后续没有数据时，触发空闲中断。举个例子：向这个缓冲区大小为 20 的程序传送一个大小为 25 的数据包，它会产生三次中断，如下图所示。

![img](https://i-blog.csdnimg.cn/blog_migrate/d126dfe820a64f2f74f9765de98e4b44.png)

原先的三个中断中的代码可以合到一个中实现：https://www.iotword.com/32826.html

> ```cs
> void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
> {
>   static uint8_t rx_buf_head = 0;
>   static uint8_t rx_size; //待处理数据长度
>  
>   rx_size = Size - rx_buf_head;
>  
>   for (uint16_t i = 0; i < rx_size; i++)
>   {
>       RxFinal[final_index++] = RxBuf[(rx_buf_head + i) % RxBufSize]; // 环形缓冲处理
>       if (final_index >= RxFinalSize) final_index = 0; // 避免 RxFinal 溢出
>   }
>  
>   rx_buf_head = rx_buf_head + rx_size;
>  
>   if (rx_buf_head >= RxBufSize) rx_buf_head = 0;
> }
> }
> ```

**串口通讯最主要的就是要不丢数据，不丢帧，基本设想就是建立一个大的串口缓冲区，串口接收到的数据使用循环队列的方式全部往这个缓冲区放，不过这种方式需要把串口缓冲区弄大一点，防止数据覆盖。在stm32中，利用DMA+空闲中断很容易做到这一点。只需要将DMA设置为循环模式，如下图**：

也就是说，在循环模式下，当DMA传输了一个数据，DMA_CNDTRx传输数量寄存器相应减一，当DMA_CNDTRx传输数量寄存器减为0时，DMA_CNDTRx寄存器将恢复为相应的初始值，不用软件干预，那么，这不就是循环队列的方式吗？
于是，只需将DMA传输方式配置为循环模式，串口接收循环队列就此完成。串口初始化代码如下：

```cs
_USART1RXBUFF RxBuff;  //定义串口接收缓冲区
 /**
  * @brief  配置嵌套向量中断控制器NVIC
  * @param  无
  * @retval 无
  */
static void NVIC_Configuration(void)
{
  NVIC_InitTypeDef NVIC_InitStructure;
  
  /* 嵌套向量中断控制器组选择 */
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
  
  /* 配置USART为中断源 */
  NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
  /* 抢断优先级*/
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
  /* 子优先级 */
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
  /* 使能中断 */
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  /* 初始化配置NVIC */
  NVIC_Init(&NVIC_InitStructure);
}
 
 /**
  * @brief  USART GPIO 配置,工作参数配置
  * @param  无
  * @retval 无
  */
void USART_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
 
    // 打开串口GPIO的时钟
    DEBUG_USART_GPIO_APBxClkCmd(DEBUG_USART_GPIO_CLK, ENABLE);
    
    // 打开串口外设的时钟
    DEBUG_USART_APBxClkCmd(DEBUG_USART_CLK, ENABLE);
 
    // 将USART Tx的GPIO配置为推挽复用模式
    GPIO_InitStructure.GPIO_Pin = DEBUG_USART_TX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DEBUG_USART_TX_GPIO_PORT, &GPIO_InitStructure);
 
  // 将USART Rx的GPIO配置为浮空输入模式
    GPIO_InitStructure.GPIO_Pin = DEBUG_USART_RX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(DEBUG_USART_RX_GPIO_PORT, &GPIO_InitStructure);
    
    // 配置串口的工作参数
    // 配置波特率
    USART_InitStructure.USART_BaudRate = DEBUG_USART_BAUDRATE;
    // 配置 针数据字长
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    // 配置停止位
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    // 配置校验位
    USART_InitStructure.USART_Parity = USART_Parity_No ;
    // 配置硬件流控制
    USART_InitStructure.USART_HardwareFlowControl = 
    USART_HardwareFlowControl_None;
    // 配置工作模式，收发一起
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    // 完成串口的初始化配置
    USART_Init(DEBUG_USARTx, &USART_InitStructure);
    
    // 串口中断优先级配置
    NVIC_Configuration();
    
    // 使能串口接收中断
    USART_ITConfig(DEBUG_USARTx, USART_IT_IDLE, ENABLE);    
    
    // 使能串口
    USART_Cmd(DEBUG_USARTx, ENABLE);        
}
/**
  * @brief  USARTx DMA 配置
  * @param  无
  * @retval 无
  */
void USARTx_DMA_Config(void)
{
        DMA_InitTypeDef DMA_InitStructure;
    
        // 开启DMA时钟
        RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
//        // 设置DMA源地址：串口数据寄存器地址*/
//        DMA_InitStructure.DMA_PeripheralBaseAddr = USART_DR_ADDRESS;
//        // 内存地址(要传输的变量的指针)
//        DMA_InitStructure.DMA_MemoryBaseAddr = (u32)SendBuff;
//        // 方向：从内存到外设    
//        DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
//        // 传输大小    
//        DMA_InitStructure.DMA_BufferSize = SENDBUFF_SIZE;
//        // 外设地址不增        
//        DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
//        // 内存地址自增
//        DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
//        // 外设数据单位    
//        DMA_InitStructure.DMA_PeripheralDataSize = 
//        DMA_PeripheralDataSize_Byte;
//        // 内存数据单位
//        DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;     
//        // DMA模式，一次或者循环模式
//        DMA_InitStructure.DMA_Mode = DMA_Mode_Normal ;
//        //DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;    
//        // 优先级：中    
//        DMA_InitStructure.DMA_Priority = DMA_Priority_Medium; 
//        // 禁止内存到内存的传输
//        DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
//        // 配置DMA通道           
//        DMA_Init(USART_TX_DMA_CHANNEL, &DMA_InitStructure);    
        
        DMA_InitStructure.DMA_PeripheralBaseAddr = USART_DR_ADDRESS;
        DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)RxBuff.rxarr;  //串口接收基地址
        DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
        DMA_InitStructure.DMA_BufferSize = RXBUFFSIZE;  //接收缓冲区的大小
        DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
        DMA_InitStructure.DMA_PeripheralDataSize = 
        DMA_PeripheralDataSize_Byte;
        DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;     
        DMA_InitStructure.DMA_Mode = DMA_Mode_Circular ;  //循环模式
        DMA_InitStructure.DMA_Priority = DMA_Priority_Medium; 
        DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
        DMA_Init(USART1RX_DMA_CHANNEL, &DMA_InitStructure);    
        // 使能DMA
//        DMA_Cmd (USART_TX_DMA_CHANNEL,ENABLE);
        DMA_Cmd (USART1RX_DMA_CHANNEL,ENABLE);
        USART_DMACmd(DEBUG_USARTx,USART_DMAReq_Rx,ENABLE);
}
```


**初始化没什么好说的，定义了串口接收缓冲区，然后DMA配置为循环模式，接收缓冲区的结构体如下：**

```cpp
#define  RXBUFFSIZE       100    //接收缓冲区的大小
/**串口接收缓冲区**/
typedef struct __USART1RXBUFF
{
    uint16_t wp;  //接收缓冲区写地址
    uint16_t rp;  //接收缓冲区的读地址
    uint8_t  rxarr[RXBUFFSIZE];  //接收缓冲区实体
}_USART1RXBUFF;
 
/**帧地址结构体**/
typedef struct __FRAMEADDR
{
    uint16_t wpx;  //本帧写地址的索引
    uint16_t rpx;  //本帧读地址的索引
}_FRAMEADDR;
 
#define  FRADDRMAX  10  //最多能记录的帧
/**帧属性结构体**/
typedef struct __FRAMEATTRI
{
    _FRAMEADDR fraddr[FRADDRMAX];  //每帧的地址
    uint8_t currfra;  //当前处理帧
    uint8_t nextfra;  //下一个帧
}_FRAMEATTRI;
```


**总共有三个结构体，结构体详细介绍如下：
首先是第一个结构体，串口接收缓冲区结构体，串口接收缓冲区RxBuff的定义就是这个结构体属性。**

```cpp
#define  RXBUFFSIZE       100    //接收缓冲区的大小
/**串口接收缓冲区**/
typedef struct __USART1RXBUFF
{
	uint16_t wp;  //接收缓冲区写地址
	uint16_t rp;  //接收缓冲区的读地址
	uint8_t  rxarr[RXBUFFSIZE];  //接收缓冲区实体
}_USART1RXBUFF;
 
```


其中 wp 记录接收缓冲区当前写到的地址， rp 记录接收缓冲区当前读到的地址，而 rxarr 则是接收缓冲区的主体，所有从串口接收到的数据都会放入这个数组中，大小为 RXBUFFSIZE 设置，这个值需要设置大一点，防止数据覆盖。图解如下：

![img](https://i-blog.csdnimg.cn/direct/6f50d038b1e3404b9d46aaf2fafbf3f5.png)

刚开始时，rp、wp都指向0，之后来了第一帧数据之后如下：

![img](https://i-blog.csdnimg.cn/direct/920eb282dfa143ab98efac5152ff3ab6.png)

**之后帧1填充至串口缓冲区，rp记录帧1的起始地址，wp记录帧1的结束地址，这两个值的记录处理如下：**

```cpp
void USART1IDLE_IRQ(void)
{
	uint16_t trnum=0;
	USART1->SR;
	USART1->DR;
	//手册虽然说这个寄存器在DMA循环模式的时候，清0之后会自动恢复为最大接收缓冲区，但加入这一步以防万一
	if(USART1RX_DMA_CHANNEL->CNDTR == 0) 
	{
		trnum = RXBUFFSIZE;
	}
	else
	{
		trnum = USART1RX_DMA_CHANNEL->CNDTR&0xffff;
	}
	
	RxBuff.wp = RXBUFFSIZE-trnum;  //得到最新帧的结束地址
	g_Fra.fraddr[g_Fra.nextfra].rpx = RxBuff.rp;  //最新帧的起始地址
	g_Fra.fraddr[g_Fra.nextfra].wpx = RxBuff.wp;  //最新帧的结束地址
	g_Fra.nextfra = (g_Fra.nextfra+1)%FRADDRMAX; //g_Fra.nextfra的值被限制再0,1....(FRADDRMAX-1)
	RxBuff.rp = RxBuff.wp;  //最新帧的起始与结束地址记录完，等待下一次记录
}
```


**知道了帧1在串口缓冲区的起始地址与结束地址，那么在主函数中就可以将数据从串口接收缓冲区取出来。等待帧1数据读取完之后，让RxBuff.rp=RxBuff.wp。如下：**

```cpp
 /**
  * @brief  JustAFra获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t JustAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;
	if(RxBuff.rp != RxBuff.wp)
	{
		rtflg = 1;
		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
		
		if(RxBuff.rp<RxBuff.wp)
		{
			for(*psize=0;*psize<(RxBuff.wp-RxBuff.rp);(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp+(*psize)];
			}
			RxBuff.rp = RxBuff.wp;
		}
		else
		{
			for((*psize)=0;RxBuff.rp<RXBUFFSIZE;RxBuff.rp++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
			}
			RxBuff.rp = 0;
			
			while(RxBuff.rp<RxBuff.wp)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
				RxBuff.rp++;
			}
		}
	}
	return rtflg;
}
```


**但是，如果仅仅这样的话，设想一下，如果帧1处理的时间比较长，此时rp=wp（数据读取的时间很短，不考虑在读取的时候又接收到下一帧这种情况），这时候后面帧2、帧3、帧4来了，而你帧1还没有处理完，如此一来，rp还是指向帧1的末尾（即帧2的起始），但wp指向的却是帧4的末尾，等下一次帧处理的时候，帧2/帧3/帧4被当成一帧了。如下图：**

![img](https://i-blog.csdnimg.cn/direct/cf50d435aad349289368e1f31f59a3ce.png)

这时候还使用上面那一种方式读取的话，帧2/3/4被当做一帧。
如何避免这种情况的发生？如果我们记录每一个到达帧的起始地址（rp）和结束地址（wp），再具备一个记录当前正在处理帧（currfra）与一个最新到达帧（nextfra），也就是说建立第二个帧属性的队列，在这个队列中记录了每个帧的属性。

![img](https://i-blog.csdnimg.cn/direct/c98011372bb64bf49b2ea58412cf204d.png)
这样的话，来了一个新的帧，即便我帧1还没有处理完，但是我可以记录帧2的属性（在串口接收缓冲区的起始地址rp与结束地址wp）、帧3、帧4；之后等帧1处理完之后，可以根据currfra的索引处理帧2、帧3、帧4，一直等待执行到currfra == nextfra，则说明所有的帧都处理完成，这种方法需要要求串口接收缓冲区足够大，不会使后面到达的帧破坏前面帧的数据。一般能同时存十个帧的大小就够用，自己估计下。
所以就使用到了后面的两个结构体：

```cpp
/**帧地址结构体**/
typedef struct __FRAMEADDR
{
	uint16_t wpx;  //本帧写地址的索引
	uint16_t rpx;  //本帧读地址的索引
}_FRAMEADDR;
 
#define  FRADDRMAX  10  //最多能记录的帧
/**帧属性结构体**/
typedef struct __FRAMEATTRI
{
	_FRAMEADDR fraddr[FRADDRMAX];  //每帧的地址，队列主体
	uint8_t currfra;  //当前处理帧
	uint8_t nextfra;  //下一个帧
}_FRAMEATTRI;
```

定义帧属性队列：

```cpp
_FRAMEATTRI g_Fra;
AI写代码cpp运行
```

全新串口空闲中断调用函数修改如下：

```cpp
/*在串口空闲中断中调用*/
void USART1IDLE_IRQ(void)
{
	uint16_t trnum=0;
	USART1->SR;
	USART1->DR;
	//手册虽然说这个寄存器在DMA循环模式的时候，清0之后会自动恢复为最大接收缓冲区，但加入这一步以防万一
	if(USART1RX_DMA_CHANNEL->CNDTR == 0) 
	{
		trnum = RXBUFFSIZE;
	}
	else
	{
		trnum = USART1RX_DMA_CHANNEL->CNDTR&0xffff;
	}
	
	RxBuff.wp = RXBUFFSIZE-trnum;  //得到最新帧的结束地址
	g_Fra.fraddr[g_Fra.nextfra].rpx = RxBuff.rp;  //最新帧的起始地址
	g_Fra.fraddr[g_Fra.nextfra].wpx = RxBuff.wp;  //最新帧的结束地址
	g_Fra.nextfra = (g_Fra.nextfra+1)%FRADDRMAX; //g_Fra.nextfra的值被限制再0,1....(FRADDRMAX-1)
	RxBuff.rp = RxBuff.wp;  //最新帧的起始与结束地址记录完，等待下一次记录
}
```

在空闲中断中，在帧属性队列主体中记录最新一帧的起始，结束地址；
获取一帧数据函数如下：

```cpp
 /**
  * @brief  GetAFra--获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t GetAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;  //返回值
	uint16_t fralen=0;  //帧长度
	if(g_Fra.currfra != g_Fra.nextfra) //如果为真，说明有未处理的帧
	{
//		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
//		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		/*根据每帧的帧属性(起始与结束地址)在串口接收缓冲区主体中获取一帧数据*/
		if(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
		{
			fralen = g_Fra.fraddr[g_Fra.currfra].wpx-g_Fra.fraddr[g_Fra.currfra].rpx;
			for((*psize)=0;(*psize)<fralen;(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx+(*psize)];
			}
			g_Fra.fraddr[g_Fra.currfra].rpx=g_Fra.fraddr[g_Fra.currfra].wpx;
		}
		else
		{
			for((*psize)=0;g_Fra.fraddr[g_Fra.currfra].rpx<RXBUFFSIZE;g_Fra.fraddr[g_Fra.currfra].rpx++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
			}
			g_Fra.fraddr[g_Fra.currfra].rpx = 0;
			
			while(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
				g_Fra.fraddr[g_Fra.currfra].rpx++;
			}
			
		}
		g_Fra.currfra = (g_Fra.currfra+1)%FRADDRMAX;
//		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		rtflg = 1;
	}
	return rtflg;
}
```

获取数据时有两种情况，一种是rp<wp，如下图所示

![img](https://i-blog.csdnimg.cn/direct/aacca9612c5b48a1b03f43255aabc418.png)
说明帧数据就在rp 与wp的地址中间，另外一种就是wp<rp,如下图所示：

![img](https://i-blog.csdnimg.cn/direct/1f12e211b4d04f20bdaf93d377ca636c.png)

数据需要分两段提取。

测试函数如下：

```cpp
flg = GetAFra(arr,&getsize);
	if(flg!=0)
	{
		if(test==8)
		{
			test = 0;
		}
		if(test == 0)
		{
			Delay(0x1ffffff);
		}
		test++;
		printf("getsize=%d\r\n",getsize);
		for(temp=0;temp<getsize;temp++)
		{
			printf("%d ",arr[temp]);
		}
		printf("\r\n");
		getsize = 0;
	}
```

就是将获取到的数据打印出来。

![img](https://i-blog.csdnimg.cn/direct/70eec3508839411592bc7210f8c8470f.png)
如图中所示：当我们获取了第一帧之后，因为加入了一个延时，模拟第一帧处理时间过长，这时候来了帧2,3,4,5，但我们能依次获取后面的4个帧。

源码如下：


bsp_usart_dma.c

```cpp
/*******************************************************
*设计：陈文德
*版本：V1.0
*******************************************************/
#include "bsp_usart_dma.h"
 
_USART1RXBUFF RxBuff;  //定义串口接收缓冲区
_FRAMEATTRI   g_Fra;
 /**
  * @brief  配置嵌套向量中断控制器NVIC
  * @param  无
  * @retval 无
  */
static void NVIC_Configuration(void)
{
  NVIC_InitTypeDef NVIC_InitStructure;
  
  /* 嵌套向量中断控制器组选择 */
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
  
  /* 配置USART为中断源 */
  NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
  /* 抢断优先级*/
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
  /* 子优先级 */
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
  /* 使能中断 */
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  /* 初始化配置NVIC */
  NVIC_Init(&NVIC_InitStructure);
}
 
 /**
  * @brief  USART GPIO 配置,工作参数配置
  * @param  无
  * @retval 无
  */
void USART_Config(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
 
	// 打开串口GPIO的时钟
	DEBUG_USART_GPIO_APBxClkCmd(DEBUG_USART_GPIO_CLK, ENABLE);
	
	// 打开串口外设的时钟
	DEBUG_USART_APBxClkCmd(DEBUG_USART_CLK, ENABLE);
 
	// 将USART Tx的GPIO配置为推挽复用模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_TX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(DEBUG_USART_TX_GPIO_PORT, &GPIO_InitStructure);
 
  // 将USART Rx的GPIO配置为浮空输入模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_RX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(DEBUG_USART_RX_GPIO_PORT, &GPIO_InitStructure);
	
	// 配置串口的工作参数
	// 配置波特率
	USART_InitStructure.USART_BaudRate = DEBUG_USART_BAUDRATE;
	// 配置 针数据字长
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	// 配置停止位
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	// 配置校验位
	USART_InitStructure.USART_Parity = USART_Parity_No ;
	// 配置硬件流控制
	USART_InitStructure.USART_HardwareFlowControl = 
	USART_HardwareFlowControl_None;
	// 配置工作模式，收发一起
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	// 完成串口的初始化配置
	USART_Init(DEBUG_USARTx, &USART_InitStructure);
	
	// 串口中断优先级配置
	NVIC_Configuration();
	
	// 使能串口接收中断
	USART_ITConfig(DEBUG_USARTx, USART_IT_IDLE, ENABLE);	
	
	// 使能串口
	USART_Cmd(DEBUG_USARTx, ENABLE);	    
}
/**
  * @brief  USARTx DMA 配置
  * @param  无
  * @retval 无
  */
void USART1_DMA_Config(void)
{
		DMA_InitTypeDef DMA_InitStructure;
	
		// 开启DMA时钟
		RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
/*******************************************************
*设计：陈文德
*版本：V1.0
*******************************************************/
#include "bsp_usart_dma.h"
 
_USART1RXBUFF RxBuff;  //定义串口接收缓冲区
_FRAMEATTRI   g_Fra;
 /**
  * @brief  配置嵌套向量中断控制器NVIC
  * @param  无
  * @retval 无
  */
static void NVIC_Configuration(void)
{
  NVIC_InitTypeDef NVIC_InitStructure;
  
  /* 嵌套向量中断控制器组选择 */
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
  
  /* 配置USART为中断源 */
  NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
  /* 抢断优先级*/
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
  /* 子优先级 */
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
  /* 使能中断 */
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  /* 初始化配置NVIC */
  NVIC_Init(&NVIC_InitStructure);
}
 
 /**
  * @brief  USART GPIO 配置,工作参数配置
  * @param  无
  * @retval 无
  */
void USART_Config(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
 
	// 打开串口GPIO的时钟
	DEBUG_USART_GPIO_APBxClkCmd(DEBUG_USART_GPIO_CLK, ENABLE);
	
	// 打开串口外设的时钟
	DEBUG_USART_APBxClkCmd(DEBUG_USART_CLK, ENABLE);
 
	// 将USART Tx的GPIO配置为推挽复用模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_TX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(DEBUG_USART_TX_GPIO_PORT, &GPIO_InitStructure);
 
  // 将USART Rx的GPIO配置为浮空输入模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_RX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(DEBUG_USART_RX_GPIO_PORT, &GPIO_InitStructure);
	
	// 配置串口的工作参数
	// 配置波特率
	USART_InitStructure.USART_BaudRate = DEBUG_USART_BAUDRATE;
	// 配置 针数据字长
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	// 配置停止位
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	// 配置校验位
	USART_InitStructure.USART_Parity = USART_Parity_No ;
	// 配置硬件流控制
	USART_InitStructure.USART_HardwareFlowControl = 
	USART_HardwareFlowControl_None;
	// 配置工作模式，收发一起
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	// 完成串口的初始化配置
	USART_Init(DEBUG_USARTx, &USART_InitStructure);
	
	// 串口中断优先级配置
	NVIC_Configuration();
	
	// 使能串口接收中断
	USART_ITConfig(DEBUG_USARTx, USART_IT_IDLE, ENABLE);	
	
	// 使能串口
	USART_Cmd(DEBUG_USARTx, ENABLE);	    
}
/**
  * @brief  USARTx DMA 配置
  * @param  无
  * @retval 无
  */
void USART1_DMA_Config(void)
{
		DMA_InitTypeDef DMA_InitStructure;
	
		// 开启DMA时钟
		RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
//		// 设置DMA源地址：串口数据寄存器地址*/
//		DMA_InitStructure.DMA_PeripheralBaseAddr = USART_DR_ADDRESS;
//		// 内存地址(要传输的变量的指针)
//		DMA_InitStructure.DMA_MemoryBaseAddr = (u32)SendBuff;
//		// 方向：从内存到外设	
//		DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
//		// 传输大小	
//		DMA_InitStructure.DMA_BufferSize = SENDBUFF_SIZE;
//		// 外设地址不增	    
//		DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
//		// 内存地址自增
//		DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
//		// 外设数据单位	
//		DMA_InitStructure.DMA_PeripheralDataSize = 
//		DMA_PeripheralDataSize_Byte;
//		// 内存数据单位
//		DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;	 
//		// DMA模式，一次或者循环模式
//		DMA_InitStructure.DMA_Mode = DMA_Mode_Normal ;
//		//DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;	
//		// 优先级：中	
//		DMA_InitStructure.DMA_Priority = DMA_Priority_Medium; 
//		// 禁止内存到内存的传输
//		DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
//		// 配置DMA通道		   
//		DMA_Init(USART_TX_DMA_CHANNEL, &DMA_InitStructure);	
		
		DMA_InitStructure.DMA_PeripheralBaseAddr = USART_DR_ADDRESS;
		DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)RxBuff.rxarr;  //串口接收基地址
		DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
		DMA_InitStructure.DMA_BufferSize = RXBUFFSIZE;  //接收缓冲区的大小
		DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
		DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
		DMA_InitStructure.DMA_PeripheralDataSize = 
		DMA_PeripheralDataSize_Byte;
		DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;	 
		DMA_InitStructure.DMA_Mode = DMA_Mode_Circular ;  //循环模式
		DMA_InitStructure.DMA_Priority = DMA_Priority_Medium; 
		DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
		DMA_Init(USART1RX_DMA_CHANNEL, &DMA_InitStructure);	
		// 使能DMA
//		DMA_Cmd (USART_TX_DMA_CHANNEL,ENABLE);
		DMA_Cmd (USART1RX_DMA_CHANNEL,ENABLE);
		USART_DMACmd(DEBUG_USARTx,USART_DMAReq_Rx,ENABLE);
}
void USART1Var_Init(void)
{
	RxBuff.rp = 0;
	RxBuff.wp = 0;
	g_Fra.currfra = 0;
	g_Fra.nextfra = 0;
}
 
void USART1_Init(void)
{
	USART1Var_Init();
	USART_Config();
	USART1_DMA_Config();
}
 
 
 /**
  * @brief  USART1IDLE_IRQ--在空闲中断中调用
  * @param  无
  * @retval 无
  */
void USART1IDLE_IRQ(void)
{
	uint16_t trnum=0;
	USART1->SR;
	USART1->DR;
	//手册虽然说这个寄存器在DMA循环模式的时候，清0之后会自动恢复为最大接收缓冲区，但加入这一步以防万一
	if(USART1RX_DMA_CHANNEL->CNDTR == 0) 
	{
		trnum = RXBUFFSIZE;
	}
	else
	{
		trnum = USART1RX_DMA_CHANNEL->CNDTR&0xffff;
	}
	
	RxBuff.wp = RXBUFFSIZE-trnum;  //得到最新帧的结束地址
	g_Fra.fraddr[g_Fra.nextfra].rpx = RxBuff.rp;  //最新帧的起始地址
	g_Fra.fraddr[g_Fra.nextfra].wpx = RxBuff.wp;  //最新帧的结束地址
	g_Fra.nextfra = (g_Fra.nextfra+1)%FRADDRMAX; //g_Fra.nextfra的值被限制再0,1....(FRADDRMAX-1)
	RxBuff.rp = RxBuff.wp;  //最新帧的起始与结束地址记录完，等待下一次记录
}
 
 /**
  * @brief  GetAFra--获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t GetAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;  //返回值
	uint16_t fralen=0;  //帧长度
	if(g_Fra.currfra != g_Fra.nextfra) //如果为真，说明有未处理的帧
	{
		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		/*根据每帧的帧属性(起始与结束地址)在串口接收缓冲区主体中获取一帧数据*/
		if(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
		{
			fralen = g_Fra.fraddr[g_Fra.currfra].wpx-g_Fra.fraddr[g_Fra.currfra].rpx;
			for((*psize)=0;(*psize)<fralen;(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx+(*psize)];
			}
			g_Fra.fraddr[g_Fra.currfra].rpx=g_Fra.fraddr[g_Fra.currfra].wpx;
		}
		else
		{
			for((*psize)=0;g_Fra.fraddr[g_Fra.currfra].rpx<RXBUFFSIZE;g_Fra.fraddr[g_Fra.currfra].rpx++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
			}
			g_Fra.fraddr[g_Fra.currfra].rpx = 0;
			
			while(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
				g_Fra.fraddr[g_Fra.currfra].rpx++;
			}
			
		}
		g_Fra.currfra = (g_Fra.currfra+1)%FRADDRMAX;
		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		rtflg = 1;
	}
	return rtflg;
}
 
 /**
  * @brief  JustAFra获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t JustAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;
	if(RxBuff.rp != RxBuff.wp)
	{
		rtflg = 1;
		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
		
		if(RxBuff.rp<RxBuff.wp)
		{
			for(*psize=0;*psize<(RxBuff.wp-RxBuff.rp);(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp+(*psize)];
			}
			RxBuff.rp = RxBuff.wp;
		}
		else
		{
			for((*psize)=0;RxBuff.rp<RXBUFFSIZE;RxBuff.rp++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
			}
			RxBuff.rp = 0;
			
			while(RxBuff.rp<RxBuff.wp)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
				RxBuff.rp++;
			}
		}
	}
	return rtflg;
}
 
		
		DMA_InitStructure.DMA_PeripheralBaseAddr = USART_DR_ADDRESS;
		DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)RxBuff.rxarr;  //串口接收基地址
		DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
		DMA_InitStructure.DMA_BufferSize = RXBUFFSIZE;  //接收缓冲区的大小
		DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
		DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
		DMA_InitStructure.DMA_PeripheralDataSize = 
		DMA_PeripheralDataSize_Byte;
		DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;	 
		DMA_InitStructure.DMA_Mode = DMA_Mode_Circular ;  //循环模式
		DMA_InitStructure.DMA_Priority = DMA_Priority_Medium; 
		DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
		DMA_Init(USART1RX_DMA_CHANNEL, &DMA_InitStructure);	
		// 使能DMA
//		DMA_Cmd (USART_TX_DMA_CHANNEL,ENABLE);
		DMA_Cmd (USART1RX_DMA_CHANNEL,ENABLE);
		USART_DMACmd(DEBUG_USARTx,USART_DMAReq_Rx,ENABLE);
}
void USART1Var_Init(void)
{
	RxBuff.rp = 0;
	RxBuff.wp = 0;
	g_Fra.currfra = 0;
	g_Fra.nextfra = 0;
}
 
void USART1_Init(void)
{
	USART1Var_Init();
	USART_Config();
	USART1_DMA_Config();
}
 
 
 /**
  * @brief  USART1IDLE_IRQ--在空闲中断中调用
  * @param  无
  * @retval 无
  */
void USART1IDLE_IRQ(void)
{
	uint16_t trnum=0;
	USART1->SR;
	USART1->DR;
	//手册虽然说这个寄存器在DMA循环模式的时候，清0之后会自动恢复为最大接收缓冲区，但加入这一步以防万一
	if(USART1RX_DMA_CHANNEL->CNDTR == 0) 
	{
		trnum = RXBUFFSIZE;
	}
	else
	{
		trnum = USART1RX_DMA_CHANNEL->CNDTR&0xffff;
	}
	
	RxBuff.wp = RXBUFFSIZE-trnum;  //得到最新帧的结束地址
	g_Fra.fraddr[g_Fra.nextfra].rpx = RxBuff.rp;  //最新帧的起始地址
	g_Fra.fraddr[g_Fra.nextfra].wpx = RxBuff.wp;  //最新帧的结束地址
	g_Fra.nextfra = (g_Fra.nextfra+1)%FRADDRMAX; //g_Fra.nextfra的值被限制再0,1....(FRADDRMAX-1)
	RxBuff.rp = RxBuff.wp;  //最新帧的起始与结束地址记录完，等待下一次记录
}
 
 /**
  * @brief  GetAFra--获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t GetAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;  //返回值
	uint16_t fralen=0;  //帧长度
	if(g_Fra.currfra != g_Fra.nextfra) //如果为真，说明有未处理的帧
	{
		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		/*根据每帧的帧属性(起始与结束地址)在串口接收缓冲区主体中获取一帧数据*/
		if(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
		{
			fralen = g_Fra.fraddr[g_Fra.currfra].wpx-g_Fra.fraddr[g_Fra.currfra].rpx;
			for((*psize)=0;(*psize)<fralen;(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx+(*psize)];
			}
			g_Fra.fraddr[g_Fra.currfra].rpx=g_Fra.fraddr[g_Fra.currfra].wpx;
		}
		else
		{
			for((*psize)=0;g_Fra.fraddr[g_Fra.currfra].rpx<RXBUFFSIZE;g_Fra.fraddr[g_Fra.currfra].rpx++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
			}
			g_Fra.fraddr[g_Fra.currfra].rpx = 0;
			
			while(g_Fra.fraddr[g_Fra.currfra].rpx<g_Fra.fraddr[g_Fra.currfra].wpx)
			{
				pbuff[(*psize)] = RxBuff.rxarr[g_Fra.fraddr[g_Fra.currfra].rpx];
				(*psize)++;
				g_Fra.fraddr[g_Fra.currfra].rpx++;
			}
			
		}
		g_Fra.currfra = (g_Fra.currfra+1)%FRADDRMAX;
		printf("currfra=%d,nextfra=%d\r\n",g_Fra.currfra,g_Fra.nextfra);
		rtflg = 1;
	}
	return rtflg;
}
 
 /**
  * @brief  JustAFra获取一帧数据
  * @param  pbuff--获取一帧数据的数组，psize--获取的数目
  * @retval rtflg--0代表没有获取数据，1代表获取到数据
  */
uint8_t JustAFra(uint8_t *pbuff,uint8_t *psize)
{
	uint8_t rtflg=0;
	if(RxBuff.rp != RxBuff.wp)
	{
		rtflg = 1;
		printf("RxBuff.rp=%d,RxBuff.wp=%d\r\n",RxBuff.rp,RxBuff.wp);
		
		if(RxBuff.rp<RxBuff.wp)
		{
			for(*psize=0;*psize<(RxBuff.wp-RxBuff.rp);(*psize)++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp+(*psize)];
			}
			RxBuff.rp = RxBuff.wp;
		}
		else
		{
			for((*psize)=0;RxBuff.rp<RXBUFFSIZE;RxBuff.rp++)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
			}
			RxBuff.rp = 0;
			
			while(RxBuff.rp<RxBuff.wp)
			{
				pbuff[(*psize)] = RxBuff.rxarr[RxBuff.rp];
				(*psize)++;
				RxBuff.rp++;
			}
		}
	}
	return rtflg;
}
 
 
 
```

bsp_usart_dma.h：

```cpp
/*******************************************************
*设计：陈文德
*版本：V1.0
*******************************************************/
#ifndef __USARTDMA_H
#define	__USARTDMA_H
#include "stm32f10x.h"
#include <stdio.h>


// 串口工作参数宏定义

#define  DEBUG_USARTx                   USART1

#define  DEBUG_USART_CLK                RCC_APB2Periph_USART1

#define  DEBUG_USART_APBxClkCmd         RCC_APB2PeriphClockCmd

#define  DEBUG_USART_BAUDRATE           115200

// USART GPIO 引脚宏定义
#define  DEBUG_USART_GPIO_CLK           (RCC_APB2Periph_GPIOA)
#define  DEBUG_USART_GPIO_APBxClkCmd    RCC_APB2PeriphClockCmd
#define  DEBUG_USART_TX_GPIO_PORT       GPIOA   
#define  DEBUG_USART_TX_GPIO_PIN        GPIO_Pin_9
#define  DEBUG_USART_RX_GPIO_PORT       GPIOA
#define  DEBUG_USART_RX_GPIO_PIN        GPIO_Pin_10
// 串口对应的DMA请求通道

#define  USART_TX_DMA_CHANNEL     DMA1_Channel4

#define  USART1RX_DMA_CHANNEL     DMA1_Channel5
// 外设寄存器地址
#define  USART_DR_ADDRESS        (USART1_BASE+0x04)
// 一次发送的数据量

#define  SENDBUFF_SIZE            500

#define  RXBUFFSIZE       50    //接收缓冲区的大小
/**串口接收缓冲区**/
typedef struct __USART1RXBUFF
{
	uint16_t wp;  //接收缓冲区写地址
	uint16_t rp;  //接收缓冲区的读地址
	uint8_t  rxarr[RXBUFFSIZE];  //接收缓冲区实体
}_USART1RXBUFF;

/**帧地址结构体**/
typedef struct __FRAMEADDR
{
	uint16_t wpx;  //本帧写地址的索引
	uint16_t rpx;  //本帧读地址的索引

}_FRAMEADDR;
#define  FRADDRMAX  10  //最多能记录的帧
/**帧属性结构体**/
typedef struct __FRAMEATTRI
{
	_FRAMEADDR fraddr[FRADDRMAX];  //每帧的地址，队列主体
	uint8_t currfra;  //当前处理帧
	uint8_t nextfra;  //下一个帧
}_FRAMEATTRI;
void USART1_Init(void);
void USART1IDLE_IRQ(void);
uint8_t GetAFra(uint8_t *pbuff,uint8_t *psize);
uint8_t JustAFra(uint8_t *pbuff,uint8_t *psize);
#endif /* __USARTDMA_H */

```

main.c

```cpp
#include "stm32f10x.h"
#include "bsp_usart_dma.h"
static void Delay(__IO u32 nCount); 

/**
  * @brief  主函数
  * @param  无
  * @retval 无
  */
int main(void)
{
	static uint8_t arr[10];
	static uint8_t getsize=0;
	static uint8_t test = 0;
	uint16_t temp;
	uint8_t flg;
	/* 初始化USART */
	USART1_Init();

	while(1)
	{
		flg = GetAFra(arr,&getsize);
		if(flg!=0)
		{
			if(test==8)
			{
				test = 0;
			}
			if(test == 0)
			{
				Delay(0x2ffffff);
			}

			test++;

			printf("getsize=%d\r\n",getsize);
			for(temp=0;temp<getsize;temp++)
			{
				printf("%d ",arr[temp]);
			}
			printf("\r\n");
			getsize = 0;
		}
	}
}

static void Delay(__IO uint32_t nCount)	 //简单的延时函数
{
	for(; nCount != 0; nCount--);
}
```







