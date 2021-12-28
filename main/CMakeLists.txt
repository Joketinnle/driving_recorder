idf_component_register(SRCS 	"main.c"
								"app_softap.c"
								"app_camera.c"
								"jpeg2avi/jpeg2avi.c"
								"jpeg2avi/list.c"
					INCLUDE_DIRS "."
								"libjpg/jpeg-9d"				
								"jpeg2avi"
					
					)
