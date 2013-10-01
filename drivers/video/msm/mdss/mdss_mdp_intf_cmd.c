/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>

#include "mdss_mdp.h"
#include "mdss_dsi.h"
#include "mdss_fb.h"
#include "mdss_panel.h"

#define VSYNC_EXPIRE_TICK 4

#define START_THRESHOLD 4
#define CONTINUE_THRESHOLD 4

#define MAX_SESSIONS 2

#define STOP_TIMEOUT msecs_to_jiffies(16 * (VSYNC_EXPIRE_TICK + 2) * 2)


struct mdss_mdp_cmd_ctx {
	struct mdss_mdp_ctl *ctl;
	u32 panel_ndx;
	u32 pp_num;
	u8 ref_cnt;
	struct completion pp_comp;
	struct completion stop_comp;
	struct list_head vsync_handlers;
	int panel_on;
	int koff_cnt;
	int clk_enabled;
	int vsync_enabled;
	int rdptr_enabled;
	struct mutex clk_mtx;
	spinlock_t clk_lock;
	struct work_struct clk_work;
	struct work_struct pp_done_work;
	atomic_t pp_done_cnt;

	/* te config */
	u8 tear_check;
	u16 height;	/* panel height */
	u16 vporch;	/* vertical porches */
	u16 start_threshold;
	u32 vclk_line;	/* vsync clock per line */
};

struct mdss_mdp_cmd_ctx mdss_mdp_cmd_ctx_list[MAX_SESSIONS];
extern char board_rev;
int get_lcd_attached(void);

static inline u32 mdss_mdp_cmd_line_count(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_mixer *mixer;
	u32 cnt = 0xffff;	/* init it to an invalid value */
	u32 init;
	u32 height;

	if (!ctl)
		goto exit;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
		if (!mixer) {
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
			goto exit;
		}
	}

	init = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL) & 0xffff;

	height = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT) & 0xffff;

	if (height < init) {
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		goto exit;
	}

	cnt = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_INT_COUNT_VAL) & 0xffff;

	if (cnt < init)		/* wrap around happened at height */
		cnt += (height - init);
	else
		cnt -= init;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	pr_debug("cnt=%d init=%d height=%d\n", cnt, init, height);
exit:
	return cnt;
}

/*
 * TE configuration:
 * dsi byte clock calculated base on 70 fps
 * around 14 ms to complete a kickoff cycle if te disabled
 * vclk_line base on 60 fps
 * write is faster than read
 * init == start == rdptr
 */
static int mdss_mdp_cmd_tearcheck_cfg(struct mdss_mdp_mixer *mixer,
			struct mdss_mdp_cmd_ctx *ctx, int enable)
{
	u32 cfg;

	cfg = BIT(19); /* VSYNC_COUNTER_EN */
	if (ctx->tear_check)
		cfg |= BIT(20);	/* VSYNC_IN_EN */
	cfg |= ctx->vclk_line;

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_VSYNC, cfg);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT,
			0xfff0); /* set to verh height */
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL,
			ctx->height);

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_RD_PTR_IRQ,
			ctx->height + 1);

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_START_POS,
			ctx->height);

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_THRESH,
		   (CONTINUE_THRESHOLD << 16) | (ctx->start_threshold));
	if (board_rev > 0x02)
		mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_TEAR_CHECK_EN, enable);

	return 0;
}

static int mdss_mdp_cmd_tearcheck_setup(struct mdss_mdp_ctl *ctl, int enable)
{
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_panel_info *pinfo;
	struct mdss_mdp_mixer *mixer;

	pinfo = &ctl->panel_data->panel_info;

	if (pinfo->mipi.vsync_enable && enable) {
		u32 mdp_vsync_clk_speed_hz, total_lines;

		mdss_mdp_vsync_clk_enable(1);

		mdp_vsync_clk_speed_hz =
		mdss_mdp_get_clk_rate(MDSS_CLK_MDP_VSYNC);
		pr_debug("%s: vsync_clk_rate=%d\n", __func__,
					mdp_vsync_clk_speed_hz);

		if (mdp_vsync_clk_speed_hz == 0) {
			pr_err("can't get clk speed\n");
			return -EINVAL;
		}

		ctx->tear_check = pinfo->mipi.hw_vsync_mode;
		ctx->height = pinfo->yres;
		ctx->vporch = pinfo->lcdc.v_back_porch +
				    pinfo->lcdc.v_front_porch +
				    pinfo->lcdc.v_pulse_width;

		/* (START_THRESHOLD * 17) / (pinfo->yres) ms kickoff window start from TE */
		ctx->start_threshold = START_THRESHOLD;

		total_lines = ctx->height + ctx->vporch;
		total_lines *= pinfo->mipi.frame_rate;
		ctx->vclk_line = mdp_vsync_clk_speed_hz / total_lines;

		pr_debug("%s: fr=%d tline=%d vcnt=%d thold=%d vrate=%d\n",
			__func__, pinfo->mipi.frame_rate, total_lines,
			ctx->vclk_line, ctx->start_threshold,
			mdp_vsync_clk_speed_hz);
	} else {
		enable = 0;
	}

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer)
		mdss_mdp_cmd_tearcheck_cfg(mixer, ctx, enable);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer)
		mdss_mdp_cmd_tearcheck_cfg(mixer, ctx, enable);

	return 0;
}

static inline void mdss_mdp_cmd_clk_on(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	mutex_lock(&ctx->clk_mtx);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	if (!ctx->clk_enabled) {
		ctx->clk_enabled = 1;
		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)1);
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);
	}
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!ctx->rdptr_enabled)
		mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
	ctx->rdptr_enabled = VSYNC_EXPIRE_TICK;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	mutex_unlock(&ctx->clk_mtx);
}

#ifdef MDP_RECOVERY 
int xx_cnt;
void mdss_dsi_ldo12_disable(void);
void mdss_dsi_pll_relock_xx(void);
#endif

static inline void mdss_mdp_cmd_clk_off(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	int set_clk_off = 0;

	mutex_lock(&ctx->clk_mtx);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!ctx->rdptr_enabled)
		set_clk_off = 1;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if ((ctx->clk_enabled && set_clk_off) || (get_lcd_attached() == 0)) {
		ctx->clk_enabled = 0;
		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
	}
	mutex_unlock(&ctx->clk_mtx);
}

static void mdss_mdp_cmd_readptr_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	pdata = ctl->panel_data;

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);

	if (!ctx) {
		pr_err("invalid ctx\n");
		return;
	}

	vsync_time = ktime_get();
	ctl->vsync_cnt++;

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x88888);
#endif

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && !tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}

	if (ctrl_pdata->mdp_busy == true) {
		spin_unlock(&ctx->clk_lock);
		return ;
	}

	if (!ctx->vsync_enabled) {
		if (ctx->rdptr_enabled)
			ctx->rdptr_enabled--;
	}

	if (ctx->rdptr_enabled == 0) {
		mdss_mdp_irq_disable_nosync
			(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
		complete(&ctx->stop_comp);
		schedule_work(&ctx->clk_work);
	}
	spin_unlock(&ctx->clk_lock);
}

#ifdef MDP_RECOVERY 
void mdss_mdp_recover_after_kickoff(void)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	int i;

	pr_info("%s: start\n", __func__);
	for (i = 0; i < MAX_SESSIONS; i++) {
		ctx = &mdss_mdp_cmd_ctx_list[i];
		if (!ctx->ctl)
			continue;
		spin_lock_irqsave(&ctx->clk_lock, flags);
		pr_info("%s: cnt=%d\n", __func__, ctx->koff_cnt);
		if (ctx->koff_cnt) {
			mdss_mdp_ctl_reset(ctx->ctl);
			pr_info("%s: intf_num=%d ctx=%p\n", __func__, ctx->ctl->intf_num, ctx);
			ctx->koff_cnt--;
			mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP,
						ctx->pp_num);
			complete_all(&ctx->pp_comp);
		}
		spin_unlock_irqrestore(&ctx->clk_lock, flags);
	}

	pr_info("%s: end\n", __func__);
}
#endif

static void mdss_mdp_cmd_pingpong_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;

#if defined (CONFIG_FB_MSM_MDSS_DBG_SEQ_TICK)
	mdss_dbg_tick_save(PP_DONE);
#endif

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}
	mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);

	complete_all(&ctx->pp_comp);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif

	if (ctx->koff_cnt) {
		atomic_inc(&ctx->pp_done_cnt);
		schedule_work(&ctx->pp_done_work);
		ctx->koff_cnt--;
		if (ctx->koff_cnt) {
			pr_err("%s: too many kickoffs=%d!\n", __func__,
			       ctx->koff_cnt);
			ctx->koff_cnt = 0;
		}
	} else
		pr_err("%s: should not have pingpong interrupt!\n", __func__);

	pr_debug("%s: ctl_num=%d intf_num=%d ctx=%d kcnt=%d\n", __func__,
		ctl->num, ctl->intf_num, ctx->pp_num, ctx->koff_cnt);

	spin_unlock(&ctx->clk_lock);
}

static void pingpong_done_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), pp_done_work);

	if (ctx->ctl)
		while (atomic_add_unless(&ctx->pp_done_cnt, -1, 0))
			mdss_mdp_ctl_notify(ctx->ctl, MDP_NOTIFY_FRAME_DONE);
}

static void clk_ctrl_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), clk_work);

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	mdss_mdp_cmd_clk_off(ctx);
}

static int mdss_mdp_cmd_add_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!handle->enabled) {
		handle->enabled = true;
		list_add(&handle->list, &ctx->vsync_handlers);
		if (!handle->cmd_post_flush)
			ctx->vsync_enabled = 1;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (!handle->cmd_post_flush)
		mdss_mdp_cmd_clk_on(ctx);

	return 0;
}

static int mdss_mdp_cmd_remove_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	struct mdss_mdp_vsync_handler *tmp;
	int num_rdptr_vsync = 0;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x88888);
#endif

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (handle->enabled) {
		handle->enabled = false;
		list_del_init(&handle->list);
	}
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (!tmp->cmd_post_flush)
			num_rdptr_vsync++;
	}
	if (!num_rdptr_vsync) {
		ctx->vsync_enabled = 0;
		ctx->rdptr_enabled = VSYNC_EXPIRE_TICK;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	return 0;
}

// call below routine mdss_mdp_ctl [QCT]
int mdss_mdp_cmd_reconfigure_splash_done(struct mdss_mdp_ctl *ctl)
{
	struct mdss_panel_data *pdata;
	int ret = 0;

	pdata = ctl->panel_data;

	pdata->panel_info.cont_splash_enabled = 0;

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_CONT_SPLASH_FINISH,
			NULL);

	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	return ret;
}

void mdp5_dump_regs(void)
{
	int i, z, start, len;
	int offsets[] = {0x0};
	int length[] = {19776};

	printk("%s: =============MDSS Reg DUMP==============\n", __func__);
	for (i = 0; i < sizeof(offsets) / sizeof(int); i++) {
		start = offsets[i];
		len = length[i];
		printk("-------- Address %05x: -------\n", start);
		for (z = 0; z < len; z++) {
			if ((z & 3) == 0)
				printk("%05x:", start + (z * 4));
			printk(" %08x", MDSS_MDP_REG_READ(start + (z * 4)));
			if ((z & 3) == 3)
				printk("\n");
		}
		if ((z & 3) != 0)
			printk("\n");
	}
	printk("%s: ============= END ==============\n", __func__);
}

static int mdss_mdp_cmd_wait4pingpong(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	int need_wait = 0;
	int rc = 0;
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	int flush_wq = (int) arg;

	pdata = ctl->panel_data;
	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);
	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->koff_cnt > 0)
		need_wait = 1;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	pr_debug("%s: need_wait=%d  intf_num=%d ctx=%p\n",
			__func__, need_wait, ctl->intf_num, ctx);

	if (need_wait) {
		if (board_rev > 0x02) {
			rc = wait_for_completion_timeout(
				&ctx->pp_comp, msecs_to_jiffies(150));
		} else {
			rc = wait_for_completion_timeout(
				&ctx->pp_comp, msecs_to_jiffies(20));
		}

		WARN(rc <= 0, "mdss_mdp_cmd_wait4pingpong timeout(%d : board_rev(%d)) ctl=%d\n",
						rc, board_rev, ctl->num);
		if (rc <= 0) {
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_FULL_HD_PT_PANEL) \
	|| defined(CONFIG_FB_MSM_MIPI_SAMSUNG_YOUM_CMD_FULL_HD_PT_PANEL)
#if defined(MDP_RECOVERY)
				dsi_send_events(ctrl_pdata, DSI_EV_MDP_FIFO_UNDERFLOW);
#else
			if (board_rev > 0x02) {
				dumpreg();
				mdp5_dump_regs();
				mdss_dsi_dump_power_clk(ctl->panel_data, 1);
				mdss_mdp_dump_power_clk();
#if defined(CONFIG_FB_MSM_MDSS_DSI_DBG)
				mdss_mdp_debug_bus();
				xlog_dump();
#endif
				//panic("mdss_mdp_cmd_wait4pingpong timeout");
			}

#endif
#else
			WARN(1, "mdss_mdp_cmd_wait4pingpong timeout");
#endif
			WARN(1, "cmd kickoff timed out (%d) ctl=%d\n",
						rc, ctl->num);
			rc = -EPERM;
			mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_TIMEOUT);
		} else {
			rc = 0;
		}
	}
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, rc);
#endif

	if (flush_wq)
		flush_work_sync(&ctx->pp_done_work);

	return rc;
}

int mdss_mdp_cmd_kickoff(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	int rc;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	if (ctx->panel_on == 0) {
		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_UNBLANK, NULL);
		WARN(rc, "intf %d unblank error (%d)\n", ctl->intf_num, rc);

		if (get_lcd_attached() == 0) {
			pr_err("%s : lcd is not attached..\n",__func__);
			return 0;
		}

		ctx->panel_on++;

		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_ON, NULL);
		WARN(rc, "intf %d panel on error (%d)\n", ctl->intf_num, rc);
	}

	/*
	 * tx dcs command if had any
	 *
	 * dsi controller start from 0
	 */

	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DSI_CMDLIST_KOFF, NULL);

	mdss_mdp_cmd_clk_on(ctx);

	INIT_COMPLETION(ctx->pp_comp);
	mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);
	mdss_mdp_ctl_write(ctl, MDSS_MDP_REG_CTL_START, 1);
	spin_lock_irqsave(&ctx->clk_lock, flags);
	ctx->koff_cnt++;
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	mb();
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	{
		void mdss_mdp_mixer_read(void);
		mdss_mdp_mixer_read();
	}
#endif

	return 0;
}

int mdss_mdp_cmd_stop(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	struct mdss_mdp_vsync_handler *tmp, *handle;
	int need_wait = 0;
	int ret = 0;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x11111);
#endif

	list_for_each_entry_safe(handle, tmp, &ctx->vsync_handlers, list)
		mdss_mdp_cmd_remove_vsync_handler(ctl, handle);

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->rdptr_enabled) {
		INIT_COMPLETION(ctx->stop_comp);
		need_wait = 1;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (need_wait)
		if (wait_for_completion_timeout(&ctx->stop_comp, STOP_TIMEOUT)
		    <= 0)
			WARN(1, "stop cmd time out\n");

	if (cancel_work_sync(&ctx->clk_work))
		pr_debug("no pending clk work\n");

	mdss_mdp_cmd_clk_off(ctx);

	flush_work_sync(&ctx->pp_done_work);

	ctx->panel_on = 0;

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   NULL, NULL);
	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   NULL, NULL);

// remove below codes, callig workqueue removed so below code is not required anymore, [QCT]
// if you don't remove, it will cancel normal workqueue
	/* Wait for workqueues to complete (if any), then clear the context */
	//cancel_work_sync(&ctx->clk_work);

	memset(ctx, 0, sizeof(*ctx));
	ctl->priv_data = NULL;

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_BLANK, NULL);
	WARN(ret, "intf %d blank error (%d)\n", ctl->intf_num, ret);

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_OFF, NULL);
	WARN(ret, "intf %d panel off error (%d)\n", ctl->intf_num, ret);

	ctl->stop_fnc = NULL;
	ctl->display_fnc = NULL;
	ctl->wait_pingpong = NULL;
	ctl->add_vsync_handler = NULL;
	ctl->remove_vsync_handler = NULL;
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x222222);
#endif
	pr_info("%s:-\n", __func__);
	return 0;
}

int mdss_mdp_cmd_start(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_mdp_mixer *mixer;
	int i, ret;

	pr_debug("%s:+\n", __func__);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		pr_err("mixer not setup correctly\n");
		return -ENODEV;
	}

	for (i = 0; i < MAX_SESSIONS; i++) {
		ctx = &mdss_mdp_cmd_ctx_list[i];
		if (ctx->ref_cnt == 0) {
			ctx->ref_cnt++;
			break;
		}
	}
	if (i == MAX_SESSIONS) {
		pr_err("too many sessions\n");
		return -ENOMEM;
	}

	ctl->priv_data = ctx;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	mdss_mdp_irq_disable(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);

	ctx->ctl = ctl;
	ctx->panel_ndx = ctl->panel_ndx;
	ctx->pp_num = mixer->num;
	init_completion(&ctx->pp_comp);
	init_completion(&ctx->stop_comp);
	spin_lock_init(&ctx->clk_lock);
	mutex_init(&ctx->clk_mtx);
	INIT_WORK(&ctx->clk_work, clk_ctrl_work);
	INIT_WORK(&ctx->pp_done_work, pingpong_done_work);
	atomic_set(&ctx->pp_done_cnt, 0);
	INIT_LIST_HEAD(&ctx->vsync_handlers);

	pr_debug("%s: ctx=%p num=%d mixer=%d\n", __func__,
			ctx, ctx->pp_num, mixer->num);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   mdss_mdp_cmd_readptr_done, ctl);

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   mdss_mdp_cmd_pingpong_done, ctl);

	ret = mdss_mdp_cmd_tearcheck_setup(ctl, 1);
	if (ret) {
		pr_err("tearcheck setup failed\n");
		return ret;
	}

	ctl->stop_fnc = mdss_mdp_cmd_stop;
	ctl->display_fnc = mdss_mdp_cmd_kickoff;
	ctl->wait_pingpong = mdss_mdp_cmd_wait4pingpong;
	ctl->add_vsync_handler = mdss_mdp_cmd_add_vsync_handler;
	ctl->remove_vsync_handler = mdss_mdp_cmd_remove_vsync_handler;
	ctl->read_line_cnt_fnc = mdss_mdp_cmd_line_count;
	pr_debug("%s:-\n", __func__);

	return 0;
}

