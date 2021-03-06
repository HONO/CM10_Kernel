#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>

#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/interrupt.h>

#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/wakelock.h> 		/* wake_lock, unlock */

#include "../../broadcast_tdmb_drv_ifdef.h"
#include "../inc/broadcast_fc8050.h"
#include "../inc/fci_types.h"
#include "../inc/bbm.h"

/* DEFINE set for TDMB driver { */
#define PM_QOS							
#define ANTENNA_SWITCHING				
#undef PMIC_CLOCK_SHARING				
#undef DELAY_USING_WAIT_EVENT_TIMEOUT	
/* DEFINE set for TDMB driver } */

#ifdef PM_QOS
#include <linux/pm_qos_params.h>
#endif  /* PM_QOS */

#ifdef ANTENNA_SWITCHING
/* for ANT. for I-pjt  */
#include <linux/mfd/pmic8058.h>
#endif  /* ANTENNA_SWITCHING */

/* external function */
extern int broadcast_drv_if_isr(void);
extern void fc8050_isr_interruptclear(void);
extern void fc8050_isr_control(fci_u8 onoff);

/* proto type declare */
static int broadcast_tdmb_fc8050_probe(struct spi_device *spi);
static int broadcast_tdmb_fc8050_remove(struct spi_device *spi);
static int broadcast_tdmb_fc8050_suspend(struct spi_device *spi, pm_message_t mesg);
static int broadcast_tdmb_fc8050_resume(struct spi_device *spi);

#ifdef ANTENNA_SWITCHING
/* ANT. switching for I-pjt */
#define PM8058_GPIO_BASE			NR_MSM_GPIOS
#define PM8058_GPIO_PM_TO_SYS(pm_gpio)		(pm_gpio + PM8058_GPIO_BASE -1)
#endif  /* ANTENNA_SWITCHING */

/* I-pjt msm8660 APQ8060 GPIOs */
#define DMB_EN 			102 //GPIO 102
#define DMB_INT_N 		107 //GPIO 107
#define DMB_RESET_N 	101 //GPIO 101

#ifdef ANTENNA_SWITCHING
#define DMB_ANT_SEL_P	PM8058_GPIO_PM_TO_SYS(11) //GPIO 11
#define DMB_ANT_SEL_N	PM8058_GPIO_PM_TO_SYS(12) //GPIO 12
#endif  /* ANTENNA_SWITCHING */

/************************************************************************/
/* LINUX Driver Setting                                                                                          */
/************************************************************************/
static uint32 user_stop_flg = 0;
struct tdmb_fc8050_ctrl_blk
{
	boolean 					TdmbPowerOnState;
	struct spi_device* 			spi_ptr;
	struct work_struct 			spi_work;
	struct workqueue_struct* 	spi_wq;
	struct mutex				mutex;
	struct wake_lock 			wake_lock;	/* wake_lock,wake_unlock */
	boolean 					spi_irq_status;
	spinlock_t                                     spin_lock;
	struct pm_qos_request_list    pm_req_list;
};


static struct tdmb_fc8050_ctrl_blk fc8050_ctrl_info;

/* FCI SPI Write Read TEST Code { */
#if 0
void tdmb_fc8050_spi_write_read_test(void)
{
	uint16 i; 
	uint32 wdata = 0; 
	uint32 ldata = 0; 
	uint32 data = 0;
	uint32 temp = 0;

#define TEST_CNT    1	
	tdmb_fc8050_power_on();

	for(i=0;i<TEST_CNT;i++)
	{
		BBM_WRITE(NULL, 0x05, i & 0xff);
		BBM_READ(NULL, 0x05, (fci_u8*)&data);
		if((i & 0xff) != data)
			printk("FC8000 byte test (0x%x,0x%x)\n", i & 0xff, data);
	}
	
	for(i=0;i<TEST_CNT;i++)
	{
		BBM_WORD_WRITE(NULL, 0x0210, i & 0xffff);
		BBM_WORD_READ(NULL, 0x0210, (fci_u16*)&wdata);
		if((i & 0xffff) != wdata)
			printk("FC8000 word test (0x%x,0x%x)\n", i & 0xffff, wdata);
	}
	
	for(i=0;i<TEST_CNT;i++)
	{
		BBM_LONG_WRITE(NULL, 0x0210, i & 0xffffffff);
		BBM_LONG_READ(NULL, 0x0210, (fci_u32*)&ldata);
		if((i & 0xffffffff) != ldata)
			printk("FC8000 long test (0x%x,0x%x)\n", i & 0xffffffff, ldata);
	}

	data = 0;

	for(i=0;i<TEST_CNT;i++)
	{
		temp = i&0xff;
		BBM_TUNER_WRITE(NULL, 0x12, 0x01, (fci_u8*)&temp, 0x01);
		BBM_TUNER_READ(NULL, 0x12, 0x01, (fci_u8*)&data, 0x01);
		if((i & 0xff) != data)
			printk("FC8000 tuner test (0x%x,0x%x)\n", i & 0xff, data);
	}
	temp = 0x51;
	BBM_TUNER_WRITE(NULL, 0x12, 0x01, (fci_u8*)&temp, 0x01 );	
	tdmb_fc8050_power_off();
}
#endif
/* FCI SPI Write Read  TEST Code } */

struct spi_device *tdmb_fc8050_get_spi_device(void)
{
	return fc8050_ctrl_info.spi_ptr;
}

void tdmb_fc8050_set_userstop(int mode)
{
	user_stop_flg = mode;
}

int tdmb_fc8050_mdelay(int32 ms)
{
	int32	wait_loop =0;
	int32	wait_ms = ms;
	int		rc = 1;  /* 0 : false, 1 : true */

	if(ms > 100)
	{
		wait_loop = (ms /100);   /* 100, 200, 300 more only , Otherwise this must be modified e.g (ms + 40)/50 */
		wait_ms = 100;
	}

	do
	{
		mdelay(wait_ms);//msleep(wait_ms);
		if(user_stop_flg == 1)
		{
			printk("~~~~~~~~ Ustop flag is set so return false ms =(%d)~~~~~~~\n", ms);
			rc = 0;
			break;
		}
	}while((--wait_loop) > 0);

	return rc;
}

void tdmb_fc8050_Must_mdelay(int32 ms)
{
	msleep(ms);
}

int tdmb_fc8050_tdmb_is_on(void)
{
	return (int)fc8050_ctrl_info.TdmbPowerOnState;
}

/* EXPORT_SYMBOL() : when we use external symbol             */
/* which is not included in current module - over kernel 2.6    */
/* EXPORT_SYMBOL(tdmb_fc8050_tdmb_is_on);                    */

int tdmb_fc8050_power_on(void)
{
	printk("tdmb_fc8050_power_on \n");
	if ( fc8050_ctrl_info.TdmbPowerOnState == FALSE )
	{
#ifdef PM_QOS
	/* QOS */
		if(pm_qos_request_active(&fc8050_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&fc8050_ctrl_info.pm_req_list, 20);	
		}
#endif  /* PM_QOS */

		wake_lock(&fc8050_ctrl_info.wake_lock);

		gpio_set_value_cansleep(DMB_ANT_SEL_P, 0);
		gpio_set_value_cansleep(DMB_ANT_SEL_N, 1);
		
		gpio_direction_input(DMB_INT_N);       
//		gpio_direction_output(DMB_RESET_N, false);
//		gpio_direction_output(DMB_EN, true);
		gpio_set_value(DMB_EN, 1);
		gpio_set_value(DMB_RESET_N, 1);
		udelay(1000);  
		udelay(1000);
		udelay(1000);
		gpio_set_value(DMB_RESET_N, 0);
		udelay(5);  
		gpio_set_value(DMB_RESET_N, 1);
		tdmb_fc8050_interrupt_free();
		fc8050_ctrl_info.TdmbPowerOnState = TRUE;

		printk("tdmb_fc8050_power_on OK\n");
		
	}
	else
	{
		printk("tdmb_fc8050_power_on the power already turn on \n");
	}

	printk("tdmb_fc8050_power_on completed \n");

	return TRUE;
}

int tdmb_fc8050_power_off(void)
{
	if ( fc8050_ctrl_info.TdmbPowerOnState == TRUE )
	{
		tdmb_fc8050_interrupt_lock();
		fc8050_ctrl_info.TdmbPowerOnState = FALSE;
		gpio_set_value(DMB_RESET_N, 0);
		gpio_set_value(DMB_EN, 0);
		gpio_direction_output(DMB_INT_N, false);   
		gpio_set_value(DMB_INT_N, 0);		

		gpio_set_value_cansleep(DMB_ANT_SEL_P, 1);	// for ESD TEST
		gpio_set_value_cansleep(DMB_ANT_SEL_N, 0);	
		wake_unlock(&fc8050_ctrl_info.wake_lock);

#ifdef PM_QOS
		/* QoS release */
		if(pm_qos_request_active(&fc8050_ctrl_info.pm_req_list)) {
			pm_qos_update_request(&fc8050_ctrl_info.pm_req_list, PM_QOS_DEFAULT_VALUE);	
		}	
#endif  /* PM_QOS */
	}
	else
	{
		printk("tdmb_fc8050_power_on the power already turn off \n");
	}	

	printk("tdmb_fc8050_power_off completed \n");
	
	return TRUE;
}

int tdmb_fc8050_select_antenna(unsigned int sel)
{
#ifdef ANTENNA_SWITCHING
	if(LGE_BROADCAST_TDMB_ANT_TYPE_INTENNA == sel)
	{

		gpio_set_value_cansleep(DMB_ANT_SEL_P, 0);
		gpio_set_value_cansleep(DMB_ANT_SEL_N, 1);
	}
	else if(LGE_BROADCAST_TDMB_ANT_TYPE_EARANT == sel)
	{
	
		gpio_set_value_cansleep(DMB_ANT_SEL_P, 1);
		gpio_set_value_cansleep(DMB_ANT_SEL_N, 0);
        
	}    
      else
	{
	      printk("fc8050_select_antenna is invalid value set = (%d)\n", sel);

		return FALSE;
	}
#endif  /* ANTENNA_SWITCHING */

      return TRUE;
}

static struct spi_driver broadcast_tdmb_driver = {
	.probe = broadcast_tdmb_fc8050_probe,
	.remove	= __devexit_p(broadcast_tdmb_fc8050_remove),
	.suspend = broadcast_tdmb_fc8050_suspend,
	.resume  = broadcast_tdmb_fc8050_resume,
	.driver = {
		.name = "tdmb_fc8050",
		.bus	= &spi_bus_type,
		.owner = THIS_MODULE,
	},
};

void tdmb_fc8050_interrupt_lock(void)
{
	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_fc8050_interrupt_lock fail\n");
	}

	disable_irq(fc8050_ctrl_info.spi_ptr->irq);
}

void tdmb_fc8050_interrupt_free(void)
{
	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_fc8050_interrupt_free fail\n");
	}

	enable_irq(fc8050_ctrl_info.spi_ptr->irq);
}

int tdmb_fc8050_spi_write_read(uint8* tx_data, int tx_length, uint8 *rx_data, int rx_length)
{
	int rc;

	struct spi_transfer	t = {
			.tx_buf		= tx_data,
			.rx_buf		= rx_data,
			.len		= tx_length+rx_length,
		};

	struct spi_message	m;	

	if (fc8050_ctrl_info.spi_ptr == NULL)
	{
		printk("tdmb_fc8050_spi_write_read error txdata=0x%x, length=%d\n", (unsigned int)tx_data, tx_length+rx_length);
	}

	mutex_lock(&fc8050_ctrl_info.mutex);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	rc = spi_sync(fc8050_ctrl_info.spi_ptr, &m);
	if ( rc < 0 )
	{
		printk("tdmb_fc8050_spi_read_burst result(%d), actual_len=%d\n",rc, m.actual_length);
	}

	mutex_unlock(&fc8050_ctrl_info.mutex);

	return TRUE;
}

static irqreturn_t broadcast_tdmb_spi_isr(int irq, void *handle)
{
	struct tdmb_fc8050_ctrl_blk* fc8050_info_p;
	unsigned long flag;

	fc8050_info_p = (struct tdmb_fc8050_ctrl_blk *)handle;	
	if ( fc8050_info_p && fc8050_info_p->TdmbPowerOnState )
	{
		if (fc8050_info_p->spi_irq_status)
		{			
			printk("######### spi read function is so late skip #########\n");			
			return IRQ_HANDLED;
		}		
//		printk("***** broadcast_tdmb_spi_isr coming *******\n");
		spin_lock_irqsave(&fc8050_info_p->spin_lock, flag);
		queue_work(fc8050_info_p->spi_wq, &fc8050_info_p->spi_work);
		spin_unlock_irqrestore(&fc8050_info_p->spin_lock, flag);    
	}
	else
	{
		printk("broadcast_tdmb_spi_isr is called, but device is off state\n");
	}

	return IRQ_HANDLED; 
}

static void broacast_tdmb_spi_work(struct work_struct *tdmb_work)
{
	struct tdmb_fc8050_ctrl_blk *pTdmbWorkData;

	pTdmbWorkData = container_of(tdmb_work, struct tdmb_fc8050_ctrl_blk, spi_work);
	if ( pTdmbWorkData )
	{
//		printk("broadcast_tdmb_spi_work START\n");
		fc8050_isr_control(0);
		pTdmbWorkData->spi_irq_status = TRUE;
		broadcast_drv_if_isr();
		pTdmbWorkData->spi_irq_status = FALSE;
		fc8050_isr_control(1);
//		printk("broadcast_tdmb_spi_work END\n");
//		printk("broacast_tdmb_spi_work is called handle=0x%x\n", (unsigned int)pTdmbWorkData);
	}
	else
	{
		printk("~~~~~~~broadcast_tdmb_spi_work call but pTdmbworkData is NULL ~~~~~~~\n");
	}
}

static int broadcast_tdmb_fc8050_probe(struct spi_device *spi)
{
	int rc;
	struct pm_gpio GPIO11_CFG = {
				.direction      = PM_GPIO_DIR_OUT,
				.pull           = PM_GPIO_PULL_NO,
				.function       = PM_GPIO_FUNC_NORMAL,
				.vin_sel        = 2,	/* for ESD TEST in I-pjt */
				.inv_int_pol    = 0,	
				};
	struct pm_gpio GPIO12_CFG = {
				.direction      = PM_GPIO_DIR_OUT,
				.pull           = PM_GPIO_PULL_NO,
				.function       = PM_GPIO_FUNC_NORMAL,
				.vin_sel        = 2,	/* for ESD TEST in I-pjt */
				.inv_int_pol    = 0,			
				};	

	fc8050_ctrl_info.TdmbPowerOnState = FALSE;
	
	fc8050_ctrl_info.spi_ptr 				= spi;
	fc8050_ctrl_info.spi_ptr->mode 			= SPI_MODE_0;
	fc8050_ctrl_info.spi_ptr->bits_per_word 	= 8;
	fc8050_ctrl_info.spi_ptr->max_speed_hz 	= ( 24000*1000 );
	rc = spi_setup(spi);
	printk("broadcast_tdmb_fc8050_probe spi_setup=%d\n", rc);
	BBM_HOSTIF_SELECT(NULL, 1);

	INIT_WORK(&fc8050_ctrl_info.spi_work, broacast_tdmb_spi_work);
	fc8050_ctrl_info.spi_wq = create_singlethread_workqueue("tdmb_spi_wq");
	if(fc8050_ctrl_info.spi_wq == NULL){
		printk("Failed to setup tdmb spi workqueue \n");
		return -ENOMEM;
	}
	rc = request_irq(spi->irq, broadcast_tdmb_spi_isr, IRQF_DISABLED | IRQF_TRIGGER_FALLING, spi->dev.driver->name, &fc8050_ctrl_info);
	printk("broadcast_tdmb_fc8050_probe request_irq=%d\n", rc);

	gpio_request(101, "DMB_RESET_N");
	gpio_request(102, "DMB_EN");
	gpio_request(107, "DMB_INT_N");
	gpio_direction_output(DMB_RESET_N, false);      
	gpio_direction_output(DMB_EN, false);               
	gpio_direction_output(DMB_INT_N, false);           

#ifdef ANTENNA_SWITCHING
	pm8xxx_gpio_config(DMB_ANT_SEL_P, &GPIO11_CFG);
	pm8xxx_gpio_config(DMB_ANT_SEL_N, &GPIO12_CFG);
	gpio_set_value_cansleep(DMB_ANT_SEL_P, 1);	/* for ESD TEST in I-pjt */
	gpio_set_value_cansleep(DMB_ANT_SEL_N, 0);	/* for ESD TEST in I-pjt */
#endif  /* ANTENNA_SWITCHING */
	tdmb_fc8050_interrupt_lock();

	mutex_init(&fc8050_ctrl_info.mutex);

	wake_lock_init(&fc8050_ctrl_info.wake_lock,  WAKE_LOCK_SUSPEND, dev_name(&spi->dev));		
	spin_lock_init(&fc8050_ctrl_info.spin_lock);

#ifdef PM_QOS
	pm_qos_add_request(&fc8050_ctrl_info.pm_req_list, PM_QOS_CPU_DMA_LATENCY, PM_QOS_DEFAULT_VALUE);
#endif  /* PM_QOS */

	printk("broadcast_fc8050_probe End\n");

	return rc;
}

static int broadcast_tdmb_fc8050_remove(struct spi_device *spi)
{
	printk("broadcast_tdmb_fc8050_remove \n");

	if (fc8050_ctrl_info.spi_wq)
	{
		flush_workqueue(fc8050_ctrl_info.spi_wq);
		destroy_workqueue(fc8050_ctrl_info.spi_wq);
	}

	free_irq(spi->irq, &fc8050_ctrl_info);

	mutex_destroy(&fc8050_ctrl_info.mutex);

	wake_lock_destroy(&fc8050_ctrl_info.wake_lock);

#ifdef PM_QOS
	pm_qos_remove_request(&fc8050_ctrl_info.pm_req_list);
#endif  /* PM_QOS */
	memset((unsigned char*)&fc8050_ctrl_info, 0x0, sizeof(struct tdmb_fc8050_ctrl_blk));
	return 0;
}

static int broadcast_tdmb_fc8050_suspend(struct spi_device *spi, pm_message_t mesg)
{
	printk("broadcast_tdmb_fc8050_suspend \n");
	return 0;
}

static int broadcast_tdmb_fc8050_resume(struct spi_device *spi)
{
	printk("broadcast_tdmb_fc8050_resume \n");
	return 0;
}

int __devinit broadcast_tdmb_drv_init(void)
{
	int rc;

	rc = broadcast_tdmb_drv_start();
	printk("broadcast_tdmb_fc8050_probe start %d\n", rc);

	return spi_register_driver(&broadcast_tdmb_driver);
}

static void __exit broadcast_tdmb_drv_exit(void)
{
	spi_unregister_driver(&broadcast_tdmb_driver);
}



module_init(broadcast_tdmb_drv_init);
module_exit(broadcast_tdmb_drv_exit);

/* optional part when we include driver code to build-on          */
/* it's just used when we make device driver to module(.ko)     */
/* so it doesn't work in build-on                                           */
MODULE_DESCRIPTION("FC8050 tdmb device driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("FCI");

