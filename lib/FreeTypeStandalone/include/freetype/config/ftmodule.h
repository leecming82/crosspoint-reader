/*
 * CrossPoint probe FreeType module list.
 *
 * Keep this intentionally narrow: TrueType + SFNT parsing + grayscale
 * renderer. The upstream full module list pulls in many unused drivers.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
