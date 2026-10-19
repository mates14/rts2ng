-- importing planets is possible from version 0.7 up

COPY targets (tar_id, type_id, tar_name, tar_ra, tar_dec, tar_comment, tar_enabled, tar_priority, tar_bonus, tar_bonus_time) FROM stdin;
190	L	Sun	\N	\N	Sun (planet)	t	0	0	\N
191	L	Mercury	\N	\N	Mercury (planet)	t	0	0	\N
192	L	Venus	\N	\N	Venus (planet)	t	0	0	\N
193	L	Moon	\N	\N	Moon (Earth)	t	0	0	\N
194	L	Mars	\N	\N	Mars (planet)	t	0	0	\N
195	L	Jupiter	\N	\N	Jupiter (planet)	t	0	0	\N
196	L	Saturn	\N	\N	Saturn (planet)	t	0	0	\N
197	L	Uranus	\N	\N	Uran (planet)	t	0	0	\N
198	L	Neptune	\N	\N	Neptun (planet)	t	0	0	\N
199	L	Pluto	\N	\N	Pluto (planet)	t	0	0	\N
\.
