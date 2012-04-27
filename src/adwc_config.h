
#define dWayland_WithPosition		1
#define dWayland_WithGlobalTransient	1

#define dXWayland_Mode	0

#define dModKey MODIFIER_ALT
/*
#define TAGKEYS(KEY,TAG) \
	((tADWC_Binding){ .Key = dModKey,				KEY,	view,		{.ui = 1 << (TAG)} }), \
	{ dModKey|ControlMask,		KEY,	toggleview,	{.ui = 1 << (TAG)} }, \
	{ dModKey|ShiftMask,		 KEY,	tag,		{.ui = 1 << (TAG)} }, \
	{ dModKey|ControlMask|ShiftMask, KEY,	toggletag,	{.ui = 1 << (TAG)} },
*/
#define TAGKEYS(KEY,TAG) \
	((tADWC_Binding){ .Mods = dModKey,	.Key = KEY,		.Handler = 0,	.Priv = 1 << (TAG)	}),


static tADWC_Binding keys[] = {
	/* modifier			key			function			argument */
/*
//	{ MODKEY,			XK_p,			spawn,			{.v = dmenucmd } },
	{ MODKEY,			XK_Return,		spawn,			{.v = termcmd } },
//	{ MODKEY,			XK_b,			togglebar,			{0} },
	{ MODKEY,			XK_Tab,		focusstack,			{.i = +1 } },
	{ MODKEY|ShiftMask,	XK_Tab,		focusstack,			{.i = -1 } },
//	{ MODKEY,			XK_h,			setmfact,			{.f = -0.05} },
//	{ MODKEY,			XK_l,			setmfact,			{.f = +0.05} },
	{ MODKEY|ShiftMask,	XK_Return,		zoom,				{0} },
//	{ MODKEY,			XK_Tab,		view,				{0} },
	
//	{ MODKEY,			XK_t,			setlayout,			{.v = &layouts[0]} },
//	{ MODKEY,			XK_f,			setlayout,			{.v = &layouts[1]} },
//	{ MODKEY,			XK_m,			setlayout,			{.v = &layouts[2]} },
//	{ MODKEY,			XK_space,		setlayout,			{0} },
	
	{ MODKEY,			XK_Insert,		togglefloating, 		{0} },
	
//	{ MODKEY,			XK_0,			view,				{.ui = ~0 } },
//	{ MODKEY|ShiftMask,	XK_0,			tag,				{.ui = ~0 } },
	
	{ MODKEY,			XK_Left,		focusmon,			{.i = -1 } },
	{ MODKEY,			XK_Right,		focusmon,			{.i = +1 } },
//	{ MODKEY|ShiftMask,	XK_comma,		tagmon,			{.i = -1 } },
//	{ MODKEY|ShiftMask,	XK_period,		tagmon,			{.i = +1 } },
	{ MODKEY,			XK_Escape,		Act_Exec,			{0} },
	
	{ MODKEY,			XK_F1,		Act_Ring_Switch,		{0} },
	{ MODKEY,			XK_F5,		Act_Ring_LinkAdd,		{0} },
	{ MODKEY,			XK_F8,		Act_Ring_LinkDel,		{0} },
	
	{ MODKEY,			XK_BackSpace,	killclient,			{0} },
	
//	{ MODKEY|ShiftMask,	XK_q,			quit,			{0} },
	
	{ MODKEY,			XK_space,		Act_Planes_Rot, 		{.i = -1} },
//	{ MODKEY|ShiftMask,	XK_space,		Act_Planes_Rot, 		{.i = +1} },
	/**/
	TAGKEYS (KEY_A,		0*3 + 0)	TAGKEYS (XK_z,		0*3 + 1)	TAGKEYS (XK_q,		0*3 + 2)
	TAGKEYS (XK_s,		1*3 + 0)	TAGKEYS (XK_x,		1*3 + 1)	TAGKEYS (XK_w,		1*3 + 2)
	TAGKEYS (XK_d,		2*3 + 0)	TAGKEYS (XK_c,		2*3 + 1)	TAGKEYS (XK_e,		2*3 + 2)
	TAGKEYS (XK_f,		3*3 + 0)	TAGKEYS (XK_v,		3*3 + 1)	TAGKEYS (XK_r,		3*3 + 2)
	TAGKEYS (XK_j,		4*3 + 0)	TAGKEYS (XK_m,		4*3 + 1)	TAGKEYS (XK_u,		4*3 + 2)
	TAGKEYS (XK_k,		5*3 + 0)	TAGKEYS (XK_comma,	5*3 + 1)	TAGKEYS (XK_i,		5*3 + 2)
	TAGKEYS (XK_l,		6*3 + 0)	TAGKEYS (XK_period,	6*3 + 1)	TAGKEYS (XK_o,		6*3 + 2)
	TAGKEYS (XK_semicolon,	7*3 + 0)	TAGKEYS (XK_slash,	7*3 + 1)	TAGKEYS (XK_p,		7*3 + 2)
	
};




