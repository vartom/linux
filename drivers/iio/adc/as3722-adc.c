#define DEBUG

#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/interrupt.h>
#include <linux/mfd/as3722.h>
#include <linux/module.h>
#include <linux/platform_device.h>

struct as3722_adc {
	struct as3722 *parent;
	struct device *dev;
	struct iio_dev *iio;

	int irq;
};

static irqreturn_t as3722_adc_irq(int irq, void *dev_id)
{
	struct as3722_adc *adc = dev_id;
	irqreturn_t err = IRQ_HANDLED;

	dev_dbg(adc->dev, "> %s(irq=%d, dev_id=%p)\n", __func__, irq, dev_id);
	dev_dbg(adc->dev, "< %s() = %d\n", __func__, err);

	return err;
}

static int as3722_adc_read_raw(struct iio_dev *iio,
			       const struct iio_chan_spec *chan,
			       int *x, int *y, long mask)
{
	struct as3722_adc *adc = iio_priv(iio);
	unsigned int value;
	int err;

	dev_dbg(adc->dev, "> %s(iio=%p, chan=%p, x=%p, y=%p, mask=%lx)\n",
		__func__, iio, chan, x, y, mask);

	err = as3722_read(adc->parent, AS3722_ADC1_MSB_RESULT_REG, &value);
	if (err < 0)
		goto out;

	*x = (value & AS3722_ADC_MSB_VAL_MASK) << 3;

	err = as3722_read(adc->parent, AS3722_ADC1_LSB_RESULT_REG, &value);
	if (err < 0)
		goto out;

	*x |= value & AS3722_ADC_LSB_VAL_MASK;

	err = IIO_VAL_INT;

out:
	dev_dbg(adc->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static const struct iio_info as3722_adc_info = {
	.read_raw = as3722_adc_read_raw,
	.driver_module = THIS_MODULE,
};

static const struct iio_chan_spec as3722_adc_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.indexed = 1,
	}, {
		.type = IIO_VOLTAGE,
		.channel = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.indexed = 1,
	},
};

static int as3722_adc_probe(struct platform_device *pdev)
{
	struct as3722_adc *adc;
	unsigned long timeout;
	struct iio_dev *iio;
	unsigned int value;
	int err;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);

	iio = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!iio) {
		err = -ENOMEM;
		goto out;
	}

	adc = iio_priv(iio);

	adc->parent = dev_get_drvdata(pdev->dev.parent);
	adc->dev = &pdev->dev;
	adc->iio = iio;

	adc->irq = platform_get_irq(pdev, 0);
	if (adc->irq < 0) {
		err = -ENXIO;
		goto out;
	}

	err = devm_request_threaded_irq(&pdev->dev, adc->irq, NULL,
					as3722_adc_irq, IRQF_ONESHOT,
					"as3722-adc", adc);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to request IRQ: %d\n", err);
		goto out;
	}

	iio->name = "as3722-adc";
	iio->dev.parent = &pdev->dev;
	iio->info = &as3722_adc_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = as3722_adc_channels;
	iio->num_channels = ARRAY_SIZE(as3722_adc_channels);

	err = devm_iio_device_register(&pdev->dev, iio);
	if (err < 0) {
		dev_err(&pdev->dev, "failed to register device: %d\n", err);
		goto out;
	}

	if (1) {
		err = as3722_update_bits(adc->parent, AS3722_ADC1_CONTROL_REG,
					 AS3722_ADC1_SOURCE_SELECT_MASK, 12);
		if (err < 0) {
			goto out;
		}

		err = as3722_update_bits(adc->parent, AS3722_ADC1_CONTROL_REG,
					 AS3722_ADC1_INTERVAL_SCAN,
					 AS3722_ADC1_INTERVAL_SCAN);
		if (err < 0) {
			goto out;
		}

		err = as3722_update_bits(adc->parent, AS3722_ADC1_CONTROL_REG,
					 AS3722_ADC1_START_CONVERSION,
					 AS3722_ADC1_START_CONVERSION);
		if (err < 0) {
			goto out;
		}
	}

	timeout = jiffies + msecs_to_jiffies(1000);

	while (time_before(jiffies, timeout)) {
		err = as3722_read(adc->parent, AS3722_ADC1_MSB_RESULT_REG,
				  &value);
		if (err < 0) {
			goto out;
		}

		if ((value & AS3722_ADC1_RESULT_NOT_READY) == 0)
			break;

		usleep_range(1000, 2000);
	}

	platform_set_drvdata(pdev, adc);

out:
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int as3722_adc_remove(struct platform_device *pdev)
{
	int err = 0;

	dev_dbg(&pdev->dev, "> %s(pdev=%p)\n", __func__, pdev);
	dev_dbg(&pdev->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static struct platform_driver as3722_adc_driver = {
	.driver = {
		.name = "as3722-adc",
	},
	.probe = as3722_adc_probe,
	.remove = as3722_adc_remove,
};
module_platform_driver(as3722_adc_driver);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("AS3722 ADC driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:as3722-adc");
