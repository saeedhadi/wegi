/*------------------------------------------------------------------
This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

A program to test EGI draw_spline functions.

Midas Zhou
midaszhou@yahoo.com
https://github.com/widora/wegi
------------------------------------------------------------------*/
#include <egi_common.h>
#include <egi_utils.h>
#include <egi_matrix.h>

int main(int argc, char **argv)
{
  int i;

  /* <<<<<<  1. EGI general init  EGI初始化流程 >>>>>> */
  /* 对不必要的一些步骤可以忽略 */

  /* 1.1 Start sys tick 	开启系统计数 */
  printf("tm_start_egitick()...\n");
  tm_start_egitick();

  /* 1.2 Start egi log 		开启日志记录 (忽略) */
  /* 1.3 Load symbol pages 	加载图形/符号映像 (忽略) */
  /* 1.4 Load freetype fonts 	加载FreeType字体 (忽略) */

  /* 1.5 Initilize sys FBDEV 	初始化FB显示设备 */
  printf("init_fbdev()...\n");
  if(init_fbdev(&gv_fb_dev))
        return -1;

  /* 1.6 Start touch read thread 启动触摸屏线程 */
  printf("Start touchread thread...\n");
  if(egi_start_touchread() !=0)
        return -1;

  /* 1.7 Set sys FB mode 	设置显示模式: 是否直接操作FB映像数据， 设置横竖屏 */
  fb_set_directFB(&gv_fb_dev,false);//true);   /* 直接操作FB映像数据,不通过FBbuffer. 播放动画时可能出现撕裂线。 */
  fb_position_rotate(&gv_fb_dev,0);   /* 横屏模式 */

  /* <<<<<  End of EGI general init EGI初始化流程结束  >>>>>> */


   /**************************************
    *  	      2. Draw Geometry 程序部分
    ***************************************/
   /* 三角形顶点坐标 */
   EGI_POINT	tripoints[3]={ {60,20}, {140,80}, {20,100} };
   /* 折线顶点坐标 */
   EGI_POINT	mpoints[6]={ {10,230}, {60,100}, {100, 190}, {180,50}, {230,120}, {300,20} };

   fbset_speed(10); /* 设置画点速度 */

#if 1
   /* 在屏幕上绘制几何图形  */
   /* 2.1 清屏 */
   clear_screen( &gv_fb_dev, WEGI_COLOR_GRAY5);

   /* 2.2 绘制线条 */
   fbset_color(WEGI_COLOR_BLACK); /* 设置画笔颜色 */
   for(i=0; i<21; i++)
	draw_wline_nc(&gv_fb_dev, 15+i*20, 10, 15+i*20, 240-10, i/3+1);	/* 绘制垂直线条 */

   /* 2.8 绘制折线 */
   fbset_color(WEGI_COLOR_FIREBRICK);
   draw_pline(&gv_fb_dev, mpoints, 6, 5);


   /* Non_parametric Spline */
   fbset_speed(0);
   fbset_color(WEGI_COLOR_GREEN);
   draw_spline(&gv_fb_dev, 6, mpoints, 0, 5);
   fbset_color(WEGI_COLOR_BLUE);
   draw_spline(&gv_fb_dev, 6, mpoints, 1, 5);
   fbset_color(WEGI_COLOR_PINK);
   draw_spline(&gv_fb_dev, 6, mpoints, 2, 5);

   fbset_color(WEGI_COLOR_WHITE);
   draw_bezier_curve(&gv_fb_dev, 6, mpoints, NULL, 1);

   fb_render(&gv_fb_dev);
   getchar();

#endif


   /* =======  Test: draw_bezier / draw_spline2 / draw_spline  ======== */
   #define TEST_SPLINE2		0
   #define TEST_BEZIER 		0
   #define TEST_BSPLINE		1

   EGI_TOUCH_DATA touch_data;
   EGI_POINT	  tchpt;	/* Touch point */
   int 		np=9;
   int		npt=-1;
   int		step;

#if 1
   /* A little pterosaur for draw_spline2() */
   EGI_POINT	  pts[9]={{89, 78}, {48, 63}, {98, 33}, {133, 117}, {218, 100}, {177, 172}, {265, 182}, {116, 198}, {92, 78} };
#else
   /* Up Going Waves */
   EGI_POINT	 pts[9]={ {10,230}, {50,100}, {90, 190}, {130, 150}, {170,180}, {210,50}, {250,120}, {280,50}, {310,10} };
#endif
   /* Weight values */
   float	  ws[9]={ 10, 10, 20, 10, 50, 0, 0, 10, 10 };  /* Put 0 to invalidate the control point */
   //float	  ws[9]={ 1, 1, 1, 1, 1, 1, 1, 1, 1 };
   //float	  ws[9]={ 2500, 2500, 2500, 2500, 2500, 2500, 2500, 2500, 2500 };

   fb_clear_workBuff(&gv_fb_dev, WEGI_COLOR_GRAY5);

   /* Turn on FB filo and set map pointer */
   fb_filo_on(&gv_fb_dev);

#if 0  /* Draw step_changing spline */
   step=20;
while(1) {
   fb_filo_flush(&gv_fb_dev); /* flush and restore old FB pixel data */

   /* change point */
   pts[4].y +=step;
   if(pts[4].y > 240) step=-20;
   else if(pts[4].y <0 ) step=20;

   /* Draw spline */
   fbset_color(WEGI_COLOR_LTBLUE);
   for(i=0; i<np; i++)
	draw_circle(&gv_fb_dev, pts[i].x, pts[i].y, 5);
   fbset_color(WEGI_COLOR_PINK);
   //draw_spline2(&gv_fb_dev, np, pts, 0, 5);
   draw_spline2(&gv_fb_dev, np, pts, 1, 5);
   fb_render(&gv_fb_dev);
}
#endif

   /* Draw spline */
   fbset_color(WEGI_COLOR_LTBLUE);
   for(i=0; i<np; i++)
	draw_circle(&gv_fb_dev, pts[i].x, pts[i].y, 5);
   fbset_color(WEGI_COLOR_PINK);
 #if TEST_SPLINE2
   //draw_spline2(&gv_fb_dev, np, pts, 0, 5);
   draw_spline2(&gv_fb_dev, np, pts, 1, 5);
 #elif TEST_BEZIER
   draw_bezier_curve(&gv_fb_dev, np, pts, NULL, 5);
 #elif TEST_BSPLINE
   draw_Bspline(&gv_fb_dev, np, pts, NULL, 3, 5);
 #else
   draw_spline(&gv_fb_dev, np, pts, 1, 5);
 #endif
   fb_render(&gv_fb_dev);

   getchar();

#if 1
   /* Realtime modifying spline */
   while(1) {
        if( egi_touch_getdata(&touch_data) ) {
                while( touch_data.status == pressing || touch_data.status == pressed_hold ) {
                        /* Adjust touch data coord. system to the same as FB pos_rotate.*/
                        //egi_touch_fbpos_data(&gv_fb_dev, &touch_data); /* Default FB and TOUCH coord NOT same! */
			/* Coord transfer */
			tchpt.x=320-1-touch_data.coord.y;
			tchpt.y=touch_data.coord.x;
//			printf("touch X=%d, Y=%d, dx=%d, dy=%d \n", tchpt.x, tchpt.y, touch_data.dx, touch_data.dy);

			/* See if touch a knob */
			if(touch_data.status == pressing) {
				for(i=0;i<np;i++) {
					if(point_incircle(&tchpt, pts+i, 15)) {
						npt=i; break;
					}
				}
				if(i==np) /* no knob touched */
					npt=-1;
			}

			/* If selected a knob, update coord.  */
			if( npt>=0 ) {
				pts[npt].x=tchpt.x;
				pts[npt].y=tchpt.y;

				/* If coincide with other knobs */
				for(i=0; i<np; i++) {
					if(i==npt)continue;
					if(point_incircle(&tchpt, pts+i, 15)) {
						pts[npt].x=pts[i].x;
						pts[npt].y=pts[i].y;
					}
				}
			}

			/* TEST ---- */
			EGI_POINT mypt={30,30};
			if(point_incircle(&tchpt, &mypt, 15)) {
				for( i=0; i<np; i++)
					printf("{%d, %d}, ",pts[i].x, pts[i].y);
				printf("\n");
			}

			/* Redraw spline */
   			fb_filo_flush(&gv_fb_dev); /* flush and restore old FB pixel data */
		   	fbset_color(WEGI_COLOR_LTBLUE);
		   	for(i=0; i<np; i++)
				draw_circle(&gv_fb_dev, pts[i].x, pts[i].y, 5);
		   	fbset_color(WEGI_COLOR_PINK);


		   #if TEST_SPLINE2
			draw_spline2(&gv_fb_dev, np, pts, 1, 5);
		   #elif  TEST_BEZIER
			fbset_color(WEGI_COLOR_GRAY2);
			for(i=0; i<np-1; i++) {
				//draw_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y,3);
				draw_dash_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y, 1, 10, 10);
				//float_draw_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y ,7,false);
			}
			fbset_color(WEGI_COLOR_PINK);
			draw_bezier_curve(&gv_fb_dev, np, pts, ws, 5);
		   #elif TEST_BSPLINE
			fbset_color(WEGI_COLOR_GRAY2);
			for(i=0; i<np-1; i++) {
				//draw_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y,3);
				draw_dash_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y, 1, 10, 10);
				//float_draw_wline(&gv_fb_dev,pts[i].x,pts[i].y,pts[i+1].x,pts[i+1].y ,7,false);
			}
			fbset_color(WEGI_COLOR_PINK);
			draw_Bspline(&gv_fb_dev, np, pts, NULL, 3, 5);
		   #else
			draw_spline(&gv_fb_dev, np, pts, 1, 5);
		   #endif
			fb_render(&gv_fb_dev);

			/* Read touch data */
			while(egi_touch_getdata(&touch_data)==false);
		}
	}
	else
	 	tm_delayms(5);

   }
#endif


  /* <<<<<  3. EGI general release EGI释放流程	 >>>>>> */
  /* 根据初始化流程做对应的释放　*/
  /* 3.1 Release sysfonts and appfonts 释放所有FreeTpype字体 (忽略) */
  /* 3.2 Release all symbol pages 释放所有图形/符号映像 (忽略) */
  /* 3.3 Release FBDEV and its data 释放FB显示设备及数据 */
  printf("fb_filo_flush() and release_fbdev()...\n");
  fb_filo_flush(&gv_fb_dev);
  release_fbdev(&gv_fb_dev);
  /* 3.4 Release virtual FBDEV 释放虚拟FB显示设备 (忽略) */
  /* 3.5 End touch read thread  结束触摸屏线程 (忽略) */
  /* 3.6 结束日志记录  (忽略) */

return 0;
}


