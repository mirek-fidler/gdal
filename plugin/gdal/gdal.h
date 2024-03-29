#ifndef _gdal_gdal_h_
#define _gdal_gdal_h_

#include <Draw/Draw.h>
#include <Geom/Geom.h>

#include <plugin/gdal/gcore/gdal_priv.h>
#include <plugin/gdal/port/cpl_conv.h>

class GDALDriver;
class GDALDataset;
class GDALRasterBand;

using namespace Upp;

class Gdal {
public:
	class Block {
	public:
		Block(Size size, GDALDataType type, byte pixel_bytes);
		
		void         GetInt(int *out, int x, int y, int count) const;
		void         GetDouble(double *out, int x, int y, int count) const;
		
	public:
		Size         size;
		GDALDataType type;
		byte         pixel_bytes;
		Buffer<byte> bytes;
	};

	class Band {
	public:
		Band(Gdal& owner, GDALRasterBand *band);
	
		Size           GetSize() const        { return size; }
		Size           GetBlockSize() const   { return block_size; }
		
		String         GetDescription() const { return band->GetDescription(); }
		String         GetUnit() const        { return band->GetUnitType(); }
		
		GDALDataType   GetType() const        { return type; }
		int            GetBytes() const       { return pixel_bytes; }
		bool           IsComplex() const      { return complex; }
		
		double         GetMinimum() const;
		double         GetMaximum() const;
		
		void           GetInt(int *out, int x, int y, int count) const;
		void           GetDouble(double *out, int x, int y, int count) const;
		int            GetInt(int x, int y) const;
		double         GetDouble(int x, int y) const;
		
		double         GetIntAt(Pointf pt) const;
		double         GetDoubleAt(Pointf pt) const;
	
	protected:
		void           LoadBlock(int column, int row) const;
		void           CalcStatistics() const;
	
	private:
		Gdal&       owner;
		GDALRasterBand *band;
		Size           size;
		Size           block_size;
		GDALDataType   type;
		bool           complex;
		bool           is_nodata;
		mutable bool   is_stat;
		mutable double stat_min;
		mutable double stat_max;
		byte           pixel_bytes;
		Size           block_count;
		int            nodata_int;
		double         nodata_value;
		mutable Buffer< One<Block> > blocks;
	};

public:
	Gdal();
	~Gdal();
	
	bool             Open(const char *filename);
	bool             OpenMemory(void *ptr, size_t len, const char *ext);
	bool             OpenMemory(const String& s, const char *ext);
	bool             IsOpen() const               { return dataset; }
	String           GetFileName() const          { return filename; }
	void             Close();

	String           GetProjection() const;

	Size             GetPixelSize() const;
	Rectf            GetExtent() const;
	const Matrixf&   GetTransform() const         { return transform; }
	const Matrixf&   GetInverseTransform() const  { return inverse_transform; }
	
	int              GetBandCount() const         { return bands.GetCount(); }
	const Band&      GetBand(int band) const      { return bands[band]; }
	const Band&      operator [] (int band) const { return bands[band]; }
	
private:
	String           filename;
	GDALDataset     *dataset;
	Array<Band>      bands;
	Matrixf          transform;
	Matrixf          inverse_transform;
};

bool SaveGeoTiff(const char *fn, const double *data, Size sz, const Rectf& extent, int epsg);

#endif
