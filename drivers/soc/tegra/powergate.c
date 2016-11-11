#define DEBUG

#include <linux/of_device.h>

#include <soc/tegra/bpmp.h>

#include <dt-bindings/soc/tegra186-powergate.h>

struct tegra_powergate {
	struct generic_pm_domain genpd;
	struct tegra_bpmp *bpmp;
	const char *name;
	unsigned int id;
};

static inline struct tegra_powergate *
to_tegra_powergate(struct generic_pm_domain *genpd)
{
	return container_of(genpd, struct tegra_powergate, genpd);
}

static bool tegra_powergate_is_powered(struct generic_pm_domain *domain)
{
	struct tegra_powergate *powergate = to_tegra_powergate(domain);
	struct mrq_pg_read_state_response response;
	struct tegra_bpmp *bpmp = powergate->bpmp;
	struct mrq_pg_read_state_request request;
	struct tegra_bpmp_message msg;
	int err;

	memset(&request, 0, sizeof(request));
	request.partition_id = powergate->id;

	memset(&response, 0, sizeof(response));

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_PG_READ_STATE;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);
	msg.rx.data = &response;
	msg.rx.size = sizeof(response);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err < 0) {
		dev_err(bpmp->dev, "failed to transfer message: %d\n", err);
		return false;
	}

	return response.logic_state != 0;
}

static int tegra_powergate_power_on(struct generic_pm_domain *domain)
{
	struct tegra_powergate *powergate = to_tegra_powergate(domain);
	struct mrq_pg_update_state_request request;
	struct tegra_bpmp *bpmp = powergate->bpmp;
	struct tegra_bpmp_message msg;
	int err = 0;

	dev_dbg(bpmp->dev, "> %s(domain=%p)\n", __func__, domain);
	dev_dbg(bpmp->dev, "  powergate: %s\n", domain->name);

	memset(&request, 0, sizeof(request));
	request.partition_id = powergate->id;
	request.sram_state = 0x1;
	request.logic_state = 0x3;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_PG_UPDATE_STATE;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err < 0) {
		dev_err(bpmp->dev, "failed to transfer message: %d\n", err);
		goto out;
	}

out:
	dev_dbg(bpmp->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static int tegra_powergate_power_off(struct generic_pm_domain *domain)
{
	struct tegra_powergate *powergate = to_tegra_powergate(domain);
	struct mrq_pg_update_state_request request;
	struct tegra_bpmp *bpmp = powergate->bpmp;
	struct tegra_bpmp_message msg;
	int err = 0;

	dev_dbg(bpmp->dev, "> %s(domain=%p)\n", __func__, domain);
	dev_dbg(bpmp->dev, "  powergate: %s\n", domain->name);

	goto out;

	memset(&request, 0, sizeof(request));
	request.partition_id = powergate->id;
	request.sram_state = 0x3;
	request.logic_state = 0x1;

	memset(&msg, 0, sizeof(msg));
	msg.mrq = MRQ_PG_UPDATE_STATE;
	msg.tx.data = &request;
	msg.tx.size = sizeof(request);

	err = tegra_bpmp_transfer(bpmp, &msg);
	if (err < 0) {
		dev_err(bpmp->dev, "failed to transfer message: %d\n", err);
		goto out;
	}

out:
	dev_dbg(bpmp->dev, "< %s() = %d\n", __func__, err);
	return err;
}

static struct generic_pm_domain *
tegra_powergate_xlate(struct of_phandle_args *spec, void *data)
{
	struct tegra_bpmp *bpmp = container_of(data, struct tegra_bpmp, genpd);
	struct generic_pm_domain *domain = ERR_PTR(-ENOENT);
	struct genpd_onecell_data *genpd = data;
	unsigned int i;

	dev_dbg(bpmp->dev, "> %s(spec=%p, data=%p)\n", __func__, spec, data);

	for (i = 0; i < genpd->num_domains; i++) {
		struct tegra_powergate *powergate;

		powergate = to_tegra_powergate(genpd->domains[i]);
		if (powergate->id == spec->args[0]) {
			domain = &powergate->genpd;
			break;
		}
	}

	dev_dbg(bpmp->dev, "< %s() = %p\n", __func__, domain);
	return domain;
}

static const struct tegra_powergate_soc tegra186_powergates[] = {
	{ "aud", TEGRA186_POWERGATE_AUD },
	{ "dfd", TEGRA186_POWERGATE_DFD },
	{ "disp", TEGRA186_POWERGATE_DISP },
	{ "dispb", TEGRA186_POWERGATE_DISPB },
	{ "dispc", TEGRA186_POWERGATE_DISPC },
	{ "ispa", TEGRA186_POWERGATE_ISPA },
	{ "nvdec", TEGRA186_POWERGATE_NVDEC },
	{ "nvjpg", TEGRA186_POWERGATE_NVJPG },
	{ "mpe", TEGRA186_POWERGATE_MPE },
	{ "pcx", TEGRA186_POWERGATE_PCX },
	{ "sax", TEGRA186_POWERGATE_SAX },
	{ "ve", TEGRA186_POWERGATE_VE },
	{ "vic", TEGRA186_POWERGATE_VIC },
	{ "xusba", TEGRA186_POWERGATE_XUSBA },
	{ "xusbb", TEGRA186_POWERGATE_XUSBB },
	{ "xusbc", TEGRA186_POWERGATE_XUSBC },
	{ "gpu", TEGRA186_POWERGATE_GPU },
	{ /* sentinel */ }
};

static const struct of_device_id tegra_powergate_ids[] = {
	{ .compatible = "nvidia,tegra186-bpmp", .data = tegra186_powergates },
	{ /* sentinel */ }
};

int tegra_bpmp_init_powergates(struct tegra_bpmp *bpmp)
{
	struct genpd_onecell_data *genpd = &bpmp->genpd;
	struct tegra_powergate *powergate;
	const struct of_device_id *match;
	unsigned int i, count;
	int err;

	match = of_match_device(tegra_powergate_ids, bpmp->dev);
	if (!match || !match->data)
		return -ENODEV;

	bpmp->powergates = match->data;

	for (count = 0; ; count++)
		if (bpmp->powergates[count].name == NULL)
			break;

	dev_dbg(bpmp->dev, "powergates:\n");

	genpd->domains = devm_kcalloc(bpmp->dev, count, sizeof(*powergate),
				      GFP_KERNEL);
	if (!genpd->domains)
		return -ENOMEM;

	genpd->xlate = tegra_powergate_xlate;
	genpd->num_domains = count;

	for (i = 0; i < count; i++) {
		const struct tegra_powergate_soc *soc = &bpmp->powergates[i];
		struct tegra_powergate *powergate;
		bool off;

		dev_dbg(bpmp->dev, "  %u: %s\n", soc->id, soc->name);

		powergate = devm_kzalloc(bpmp->dev, sizeof(*powergate),
					 GFP_KERNEL);
		if (!powergate)
			return -ENOMEM;

		powergate->genpd.name = soc->name;
		powergate->genpd.power_off = tegra_powergate_power_off;
		powergate->genpd.power_on = tegra_powergate_power_on;

		powergate->id = soc->id;
		powergate->bpmp = bpmp;

		off = !tegra_powergate_is_powered(&powergate->genpd);

		dev_dbg(bpmp->dev, "    partition is %s\n", off ? "off" : "on");

		pm_genpd_init(&powergate->genpd, NULL, off);

		/* XXX */
		//if (powergate->id != TEGRA186_POWERGATE_DFD)
		if ((powergate->id == TEGRA186_POWERGATE_DISP) ||
		    (powergate->id == TEGRA186_POWERGATE_DISPB) ||
		    (powergate->id == TEGRA186_POWERGATE_DISPC))
			tegra_powergate_power_on(&powergate->genpd);

		genpd->domains[i] = &powergate->genpd;
	}

	err = of_genpd_add_provider_onecell(bpmp->dev->of_node, genpd);
	if (err < 0) {
		dev_err(bpmp->dev, "failed to register genpd provider: %d\n",
			err);
		return err;
	}

	return 0;
}
