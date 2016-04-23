/*
 * Driver for Cubiescreen touchscreens
 *
 * Copyright (c) 2016 y.salnikov
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>


#define MAX_POINTS 	2

#define TOUCH_NUM_REG	0x6D
#define X1_H_REG	0x44
#define X1_L_REG	0x40
#define Y1_H_REG	0x45
#define Y1_L_REG	0x41
#define DX1_REG		0x4f
#define DY1_REG		0x51

#define X2_H_REG	0x46
#define X2_L_REG	0x42
#define Y2_H_REG	0x47
#define Y2_L_REG	0x43
#define DX2_REG		0x50
#define DY2_REG		0x52


struct cubiets_ts {
	struct i2c_client	*client;
	struct input_dev	*input;
	const struct cubiets_ts_platdata *pdata;
	char			phys[32];

	wait_queue_head_t	wait;
	bool			stopped;
};

struct cub_point_t {
	int	coord_x;
	int	coord_y;
	int	area_major;
	int	area_minor;
	int	orientation;
};

struct cubiets_ts_platdata {

        unsigned int x_max;
        unsigned int y_max;
};



void cubiets_report_point(struct cubiets_ts *ts, struct cub_point_t *point)
{
	input_report_abs(ts->input, ABS_MT_POSITION_X, point->coord_x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y,point->coord_y);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,point->area_major);
	input_report_abs(ts->input, ABS_MT_TOUCH_MINOR,point->area_minor);
	input_report_abs(ts->input, ABS_MT_ORIENTATION,point->orientation);
	input_mt_sync(ts->input);
}



static irqreturn_t cubiets_interrupt_thread(int irq, void *dev_id)
{
	struct cubiets_ts *ts = dev_id;
	char touch_num,adr,resp[2],DX1,DY1,DX2,DY2;
	int X1,Y1,X2,Y2,i;
	struct cub_point_t	point[2];

	if(!(ts->stopped))
	{
		adr=TOUCH_NUM_REG;
		i2c_master_send(ts->client,&adr,1);
		i2c_master_recv(ts->client,&touch_num,1);
		if(touch_num > MAX_POINTS) touch_num=MAX_POINTS;
		switch(touch_num)
		{
			case 2:
				adr=X2_H_REG; i2c_master_send(ts->client,&adr,1);
				i2c_master_recv(ts->client,resp,2);               // X2_H, Y2_H
				X2=(resp[0]&0x0f)<<8; Y2=(resp[1]&0x0f)<<8;
				adr=X2_L_REG; i2c_master_send(ts->client,&adr,1);
				i2c_master_recv(ts->client,resp,2);               // X2_L, Y2_L
				X2|=resp[0]; Y2|=resp[1];
				adr=DX2_REG; i2c_master_send(ts->client,&adr,1);  //DX2
				i2c_master_recv(ts->client,&DX2,1);
				adr=DY2_REG; i2c_master_send(ts->client,&adr,1);  //DY2
				i2c_master_recv(ts->client,&DY2,1);
				point[1].coord_x=X2; point[1].coord_y=Y2; 
				point[1].area_major = max(DX2, DY2);
				point[1].area_minor = min(DX2, DY2);
				point[1].orientation = DX2 > DY2;

			case 1:	
				adr=X1_H_REG; i2c_master_send(ts->client,&adr,1);
				i2c_master_recv(ts->client,resp,2);               // X1_H, Y1_H
				X1=(resp[0]&0x0f)<<8; Y1=(resp[1]&0x0f)<<8;
				adr=X1_L_REG; i2c_master_send(ts->client,&adr,1);
				i2c_master_recv(ts->client,resp,2);               // X1_L, Y1_L
				X1|=resp[0]; Y1|=resp[1];
				adr=DX1_REG; i2c_master_send(ts->client,&adr,1);  //DX1
				i2c_master_recv(ts->client,&DX1,1);
				adr=DY1_REG; i2c_master_send(ts->client,&adr,1);  //DY1
				i2c_master_recv(ts->client,&DY1,1);
				point[0].coord_x=X1; point[0].coord_y=Y1; 
				point[0].area_major = max(DX1, DY1);
				point[0].area_minor = min(DX1, DY1);
				point[0].orientation = DX1 > DY1;

			
		}
		i=0;
		for(i=0;i<touch_num;i++)
		{
			cubiets_report_point(ts,&point[i]);

		}
		if(touch_num>0)
		{
			input_report_key(ts->input, BTN_TOUCH, 1);
			input_report_abs(ts->input, ABS_X, point[0].coord_x);
			input_report_abs(ts->input, ABS_Y, point[0].coord_y);
			
		}
		else
		{
			input_report_key(ts->input, BTN_TOUCH, 0);
		}
		input_sync(ts->input);
		
	}
	return IRQ_HANDLED;
}


static int cubiets_start(struct cubiets_ts *ts)
{
	ts->stopped = false;
	return 0;
}

static int cubiets_stop(struct cubiets_ts *ts)
{
	ts->stopped = true;
	return 0;
}

static int cubiets_input_open(struct input_dev *dev)
{
	struct cubiets_ts *ts = input_get_drvdata(dev);

	return cubiets_start(ts);
}

static void cubiets_input_close(struct input_dev *dev)
{
	struct cubiets_ts *ts = input_get_drvdata(dev);

	cubiets_stop(ts);

	return;
}



#ifdef CONFIG_OF
static struct cubiets_ts_platdata *cubiets_parse_dt(struct device *dev)
{
	struct cubiets_ts_platdata *pdata;
	struct device_node *np = dev->of_node;

	if (!np)
		return ERR_PTR(-ENOENT);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "failed to allocate platform data\n");
		return ERR_PTR(-ENOMEM);
	}


	if (of_property_read_u32(np, "x-size", &pdata->x_max)) {
		dev_err(dev, "failed to get x-size property\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_property_read_u32(np, "y-size", &pdata->y_max)) {
		dev_err(dev, "failed to get y-size property\n");
		return ERR_PTR(-EINVAL);
	}

	return pdata;
}
#else
static struct cubiets_ts_platdata *cubiets_parse_dt(struct device *dev)
{
	return ERR_PTR(-EINVAL);
}
#endif


static int cubiets_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	const struct cubiets_ts_platdata *pdata;
	struct cubiets_ts *ts;
	struct input_dev *input_dev;
	int error;

	pdata = dev_get_platdata(&client->dev);
	if (!pdata) {
		pdata = cubiets_parse_dt(&client->dev);
		if (IS_ERR(pdata))
			return PTR_ERR(pdata);
	}

	ts = devm_kzalloc(&client->dev,
			  sizeof(struct cubiets_ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	input_dev = devm_input_allocate_device(&client->dev);
	if (!input_dev) {
		dev_err(&client->dev, "could not allocate input device\n");
		return -ENOMEM;
	}

	ts->pdata = pdata;
	ts->client = client;
	ts->input = input_dev;
	ts->stopped = true;
	init_waitqueue_head(&ts->wait);

	snprintf(ts->phys, sizeof(ts->phys),
		 "%s/input0", dev_name(&client->dev));

	input_dev->name = "Cubiescreen Touchscreen";
	input_dev->phys = ts->phys;
	input_dev->id.bustype = BUS_I2C;

	input_dev->open = cubiets_input_open;
	input_dev->close = cubiets_input_close;

	set_bit(EV_ABS, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);

	/* For single touch */
	input_set_abs_params(input_dev, ABS_X, 0, pdata->x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, pdata->y_max, 0, 0);

	/* For multi touch */
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0,
			     pdata->x_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0,
			     pdata->y_max, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0,
			     30, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR, 0,
			     30, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_ORIENTATION, 0, 1, 0, 0);
	

	input_set_drvdata(ts->input, ts);


	msleep(200);


	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, cubiets_interrupt_thread,
					  IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					  input_dev->name, ts);
	if (error) {
		dev_err(&client->dev, "irq %d requested failed, %d\n",
			client->irq, error);
		return error;
	}

	/* stop device and put it into deep sleep until it is opened */
	error = cubiets_stop(ts);
	if (error)
		return error;

	error = input_register_device(input_dev);
	if (error) {
		dev_err(&client->dev, "could not register input device, %d\n",
			error);
		return error;
	}

	i2c_set_clientdata(client, ts);
	
	return 0;
}

static const struct i2c_device_id cubiets_idtable[] = {
	{ "cubiets", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cubiets_idtable);

#ifdef CONFIG_OF
static const struct of_device_id cubiets_ts_dt_idtable[] = {
	{ .compatible = "cubiets" },
	{},
};
MODULE_DEVICE_TABLE(of, cubiets_ts_dt_idtable);
#endif

static struct i2c_driver cubiets_driver = {
	.driver = {
		.name	= "cubiets",
		.of_match_table	= of_match_ptr(cubiets_ts_dt_idtable),
	},
	.probe		= cubiets_probe,
	.id_table	= cubiets_idtable,
};

module_i2c_driver(cubiets_driver);

MODULE_DESCRIPTION("Cubiescreen touchscreen driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Yaroslav Salnikov <y.salnikov65535@gmail.com>");
