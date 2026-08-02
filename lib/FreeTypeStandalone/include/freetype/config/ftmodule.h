/*
 * CrossPoint probe FreeType module list.
 *
 * Keep this intentionally narrow: TrueType + SFNT parsing + grayscale and
 * monochrome renderers. The upstream full module list pulls in many unused
 * drivers.
 *
 * ft_raster1_renderer_class is the monochrome rasterizer, and the only place
 * dropout control lives. Without it FT_RENDER_MODE_MONO fails every glyph with
 * FT_Err_Cannot_Render_Glyph (19); it is not merely slower or coarser, it is
 * absent. Grayscale rendering continues to use the smooth renderer below, so
 * adding this changes nothing for callers that do not ask for MONO.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
