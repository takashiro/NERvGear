#pragma once

#include "vglobal.h"
#include "VVector.h"
#include "VMatrix.h"
#include <assert.h>
#include <stdlib.h>
#include "VConstants.h"


namespace NervGear {

// Forward-declare our templates.
template<class T> class VR4Matrix;

template<class T> class VR3Matrix;

template<class T> class PoseState;

template<> struct VCompatibleTypes<VQuat<float> >     { typedef ovrQuatf Type; };
template<> struct VCompatibleTypes<VQuat<double> >    { typedef ovrQuatd Type; };
template<> struct VCompatibleTypes<VPos<float> >     { typedef ovrPosef Type; };
template<> struct VCompatibleTypes<PoseState<float> >{ typedef ovrPoseStatef Type; };
//-------------------------------------------------------------------------------------
// ***** VBox

// Bounds class used to describe a 3D axis aligned bounding box.


// ***** VQuat
//
// VQuatf represents a VQuaternion class used for rotations.
//
// VQuaternion multiplications are done in right-to-left order, to match the
// behavior of matrices.

template<class T>
class VQuat
{
public:
	T x, y, z, w;

	VQuat() : x(0), y(0), z(0), w(1) { }
	VQuat(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) { }
	explicit VQuat(const VQuat<typename VConstants<T>::VdifFloat> &src)
		: x((T)src.x), y((T)src.y), z((T)src.z), w((T)src.w) { }


	// Constructs VQuaternion for rotation around the VAxis by an VAngle.
	VQuat(const V3Vect<T>& VAxis, T VAngle)
	{
		// Make sure we don't divide by zero.
		if (VAxis.LengthSq() == 0)
		{
			// Assert if the VAxis is zero, but the VAngle isn't
			OVR_ASSERT(VAngle == 0);
			x = 0; y = 0; z = 0; w = 1;
			return;
		}

		V3Vect<T> unitVAxis = VAxis.Normalized();
		T          sinHalfVAngle = sin(VAngle * T(0.5));

		w = cos(VAngle * T(0.5));
		x = unitVAxis.x * sinHalfVAngle;
		y = unitVAxis.y * sinHalfVAngle;
		z = unitVAxis.z * sinHalfVAngle;
	}

	// Constructs VQuaternion for rotation around one of the coordinate VAxis by an VAngle.
	VQuat(VAxis A, T VAngle, VRotateDirection d = VRotate_CCW, VHandedSystem s = VHanded_R)
	{
		T sinHalfVAngle = s * d * sin(VAngle * T(0.5));
		T v[3];
		v[0] = v[1] = v[2] = T(0);
		v[A] = sinHalfVAngle;

		w = cos(VAngle * T(0.5));
		x = v[0];
		y = v[1];
		z = v[2];
	}

	// Constructs the VQuaternion from a rotation matrix
	explicit VQuat(const VR4Matrix<T>& m)
	{
		T trace = m.M[0][0] + m.M[1][1] + m.M[2][2];

		// In almost all cases, the first part is executed.
		// However, if the trace is not positive, the other
		// cases arise.
		if (trace > T(0))
		{
			T s = sqrt(trace + T(1)) * T(2); // s=4*qw
			w = T(0.25) * s;
			x = (m.M[2][1] - m.M[1][2]) / s;
			y = (m.M[0][2] - m.M[2][0]) / s;
			z = (m.M[1][0] - m.M[0][1]) / s;
		}
		else if ((m.M[0][0] > m.M[1][1])&&(m.M[0][0] > m.M[2][2]))
		{
			T s = sqrt(T(1) + m.M[0][0] - m.M[1][1] - m.M[2][2]) * T(2);
			w = (m.M[2][1] - m.M[1][2]) / s;
			x = T(0.25) * s;
			y = (m.M[0][1] + m.M[1][0]) / s;
			z = (m.M[2][0] + m.M[0][2]) / s;
		}
		else if (m.M[1][1] > m.M[2][2])
		{
			T s = sqrt(T(1) + m.M[1][1] - m.M[0][0] - m.M[2][2]) * T(2); // S=4*qy
			w = (m.M[0][2] - m.M[2][0]) / s;
			x = (m.M[0][1] + m.M[1][0]) / s;
			y = T(0.25) * s;
			z = (m.M[1][2] + m.M[2][1]) / s;
		}
		else
		{
			T s = sqrt(T(1) + m.M[2][2] - m.M[0][0] - m.M[1][1]) * T(2); // S=4*qz
			w = (m.M[1][0] - m.M[0][1]) / s;
			x = (m.M[0][2] + m.M[2][0]) / s;
			y = (m.M[1][2] + m.M[2][1]) / s;
			z = T(0.25) * s;
		}
	}

	// Constructs the VQuaternion from a rotation matrix
	explicit VQuat(const VR3Matrix<T>& m)
	{
		T trace = m.M[0][0] + m.M[1][1] + m.M[2][2];

		// In almost all cases, the first part is executed.
		// However, if the trace is not positive, the other
		// cases arise.
		if (trace > T(0))
		{
			T s = sqrt(trace + T(1)) * T(2); // s=4*qw
			w = T(0.25) * s;
			x = (m.M[2][1] - m.M[1][2]) / s;
			y = (m.M[0][2] - m.M[2][0]) / s;
			z = (m.M[1][0] - m.M[0][1]) / s;
		}
		else if ((m.M[0][0] > m.M[1][1])&&(m.M[0][0] > m.M[2][2]))
		{
			T s = sqrt(T(1) + m.M[0][0] - m.M[1][1] - m.M[2][2]) * T(2);
			w = (m.M[2][1] - m.M[1][2]) / s;
			x = T(0.25) * s;
			y = (m.M[0][1] + m.M[1][0]) / s;
			z = (m.M[2][0] + m.M[0][2]) / s;
		}
		else if (m.M[1][1] > m.M[2][2])
		{
			T s = sqrt(T(1) + m.M[1][1] - m.M[0][0] - m.M[2][2]) * T(2); // S=4*qy
			w = (m.M[0][2] - m.M[2][0]) / s;
			x = (m.M[0][1] + m.M[1][0]) / s;
			y = T(0.25) * s;
			z = (m.M[1][2] + m.M[2][1]) / s;
		}
		else
		{
			T s = sqrt(T(1) + m.M[2][2] - m.M[0][0] - m.M[1][1]) * T(2); // S=4*qz
			w = (m.M[1][0] - m.M[0][1]) / s;
			x = (m.M[0][2] + m.M[2][0]) / s;
			y = (m.M[1][2] + m.M[2][1]) / s;
			z = T(0.25) * s;
		}
	}

	// Constructs a VQuaternion that rotates 'from' to line up with 'to'.
	explicit VQuat( const V3Vect<T> & from, const V3Vect<T> & to )
	{
		const T cx = from.y * to.z - from.z * to.y;
		const T cy = from.z * to.x - from.x * to.z;
		const T cz = from.x * to.y - from.y * to.x;
		const T dot = from.x * to.x + from.y * to.y + from.z * to.z;
		const T crossLengthSq = cx * cx + cy * cy + cz * cz;
		const T magnitude = sqrt( crossLengthSq + dot * dot );
		const T cw = dot + magnitude;
		if ( cw < VConstants<T>::SmallestNonDenormal )
		{
			const T sx = to.y * to.y + to.z * to.z;
			const T sz = to.x * to.x + to.y * to.y;
			if ( sx > sz )
			{
				const T rcpLength = RcpSqrt( sx );
				x = T(0);
				y = to.z * rcpLength;
				z = - to.y * rcpLength;
				w = T(0);
			}
			else
			{
				const T rcpLength = RcpSqrt( sz );
				x = to.y * rcpLength;
				y = - to.x * rcpLength;
				z = T(0);
				w = T(0);
			}
			return;
		}
		const T rcpLength = RcpSqrt( crossLengthSq + cw * cw );
		x = cx * rcpLength;
		y = cy * rcpLength;
		z = cz * rcpLength;
		w = cw * rcpLength;
	}

  // C-interop support.
	VQuat(const typename VCompatibleTypes<VQuat<T> >::Type& s) : x(s.x), y(s.y), z(s.z), w(s.w) { }

	operator const typename VCompatibleTypes<VQuat<T> >::Type () const
	{
		typename VCompatibleTypes<VQuat<T> >::Type result;
		result.x = x;
		result.y = y;
		result.z = z;
		result.w = w;
		return result;
	}



	    bool operator== (const VQuat& b) const   { return x == b.x && y == b.y && z == b.z && w == b.w; }
	    bool operator!= (const VQuat& b) const   { return x != b.x || y != b.y || z != b.z || w != b.w; }

	    VQuat  operator+  (const VQuat& b) const  { return VQuat(x + b.x, y + b.y, z + b.z, w + b.w); }
	    VQuat& operator+= (const VQuat& b)        { w += b.w; x += b.x; y += b.y; z += b.z; return *this; }
	    VQuat  operator-  (const VQuat& b) const  { return VQuat(x - b.x, y - b.y, z - b.z, w - b.w); }
	    VQuat& operator-= (const VQuat& b)        { w -= b.w; x -= b.x; y -= b.y; z -= b.z; return *this; }

	    VQuat  operator*  (T s) const            { return VQuat(x * s, y * s, z * s, w * s); }
	    VQuat& operator*= (T s)                  { w *= s; x *= s; y *= s; z *= s; return *this; }
	    VQuat  operator/  (T s) const            { T rcp = T(1)/s; return VQuat(x * rcp, y * rcp, z * rcp, w *rcp); }
	    VQuat& operator/= (T s)                  { T rcp = T(1)/s; w *= rcp; x *= rcp; y *= rcp; z *= rcp; return *this; }


	    // Get Imaginary part vector
	    V3Vect<T> Imag() const                 { return V3Vect<T>(x,y,z); }

	    // Get VQuaternion length.
	    T       Length() const                  { return sqrt(LengthSq()); }

	    // Get VQuaternion length squared.
	    T       LengthSq() const                { return (x * x + y * y + z * z + w * w); }

	    // Simple Euclidean distance in R^4 (not SLERP distance, but at least respects Haar measure)
	    T       Distance(const VQuat& q) const
		{
	        T d1 = (*this - q).Length();
	        T d2 = (*this + q).Length(); // Antipodal point check
	        return (d1 < d2) ? d1 : d2;
		}

	    T       DistanceSq(const VQuat& q) const
	    {
	        T d1 = (*this - q).LengthSq();
	        T d2 = (*this + q).LengthSq(); // Antipodal point check
	        return (d1 < d2) ? d1 : d2;
	    }

	    T       Dot(const VQuat& q) const
	    {
	        return x * q.x + y * q.y + z * q.z + w * q.w;
	    }

		// VAngle between two VQuaternions in radians
	    T       VAngle(const VQuat& q) const
		{
			return 2 * VArccos(fabs(Dot(q)));
		}

		// Determine if this a unit VQuaternion.
	    bool    IsNormalized() const            { return fabs(LengthSq() - T(1)) < VConstants<T>::Tolerance; }

	    // Normalize
	    void    Normalize()
		{
	#if 0	// FIXME: use this safe normalization instead
			T l = LengthSq();
			*this *= RcpSqrt(l);
	#else
	 		T l = Length();
			*this /= l;
	#endif
		}

	    // Returns normalized (unit) version of the VQuaternion without modifying itself.
		VQuat    Normalized() const
		{
	#if 0	// FIXME: use this safe normalization instead
			T l = LengthSq();
			return *this * RcpSqrt(l);
	#else
			T l = Length();
			return *this / l;
	#endif
		}

	    // Returns conjugate of the VQuaternion. Produces inverse rotation if VQuaternion is normalized.
	    VQuat    Conj() const                    { return VQuat(-x, -y, -z, w); }

	    // VQuaternion multiplication. Combines VQuaternion rotations, performing the one on the
	    // right hand side first.
	    VQuat  operator* (const VQuat& b) const   { return VQuat(w * b.x + x * b.w + y * b.z - z * b.y,
	                                                          w * b.y - x * b.z + y * b.w + z * b.x,
	                                                          w * b.z + x * b.y - y * b.x + z * b.w,
	                                                          w * b.w - x * b.x - y * b.y - z * b.z); }
		V3Vect<T> operator* (const V3Vect<T> & v) const
		{
			return Rotate(v);
		}

	    // this^p normalized; same as rotating by this p times.
	    VQuat PowNormalized(T p) const
	    {
	        V3Vect<T> v;
	        T          a;
	        GetAxisAngle(&v, &a);
	        return VQuat(v, a * p);
	    }

	    // Normalized linear interpolation of VQuaternions
	    // FIXME: This is opposite of Lerp for some reason.  It goes from 1 to 0 instead of 0 to 1.  Leaving it as a gift for future generations to deal with.
	    VQuat Nlerp(const VQuat& other, T a) const
	    {
	        T sign = (Dot(other) >= 0) ? 1 : -1;
	        return (*this * sign * a + other * (1-a)).Normalized();
	    }

	    // Rotate transforms vector in a manner that matches Matrix rotations (counter-clockwise,
	    // assuming negative diVRection of the VAxis). Standard formula: q(t) * V * q(t)^-1.
	    V3Vect<T> Rotate(const V3Vect<T>& v) const
	    {
	        return ((*this * VQuat<T>(v.x, v.y, v.z, T(0))) * Inverted()).Imag();
	    }

	    // Inversed VQuaternion rotates in the opposite diVRection.
	    VQuat        Inverted() const
	    {
	        return VQuat(-x, -y, -z, w);
	    }

	    // Sets this VQuaternion to the one rotates in the opposite diVRection.
	    void        Invert()
	    {
	        *this = VQuat(-x, -y, -z, w);
	    }

	    // Compute VAxis and VAngle from VQuaternion
	    void GetAxisAngle(V3Vect<T>* VAxis, T* VAngle) const
	    {
			if ( x*x + y*y + z*z > VConstants<T>::Tolerance * VConstants<T>::Tolerance ) {
				*VAxis  = V3Vect<T>(x, y, z).Normalized();
				*VAngle = 2 * VArccos(w);
				if (*VAngle > VConstants<T>::Pi) // Reduce the magnitude of the VAngle, if necessary
				{
					*VAngle = VConstants<T>::Pi*2 - *VAngle;
					*VAxis = *VAxis * static_cast<T>(-1);
				}
			}
			else
			{
				*VAxis = V3Vect<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
				*VAngle= 0;
			}
	    }

		// GetEulerVAngles extracts Euler VAngles from the VQuaternion, in the specified order of
	    // VAxis rotations and the specified coordinate system. Right-handed coordinate system
	    // is the default, with CCW rotations while looking in the negative VAxis diVRection.
	    // Here a,b,c, are the Yaw/Pitch/Roll VAngles to be returned.
	    // rotation a around VAxis A1
	    // is followed by rotation b around VAxis A2
	    // is followed by rotation c around VAxis A3
	    // rotations are CCW or CW (D) in LH or RH coordinate system (S)
	    template <VAxis A1, VAxis A2, VAxis A3, VRotateDirection D, VHandedSystem S>
	    void GetEulerAngles(T *a, T *b, T *c) const
	    {
	        OVR_COMPILER_ASSERT((A1 != A2) && (A2 != A3) && (A1 != A3));

	        T Q[3] = { x, y, z };  //VQuaternion components x,y,z

	        T ww  = w*w;
	        T Q11 = Q[A1]*Q[A1];
	        T Q22 = Q[A2]*Q[A2];
	        T Q33 = Q[A3]*Q[A3];

	        T psign = T(-1);
	        // Determine whether even permutation
	        if (((A1 + 1) % 3 == A2) && ((A2 + 1) % 3 == A3))
	            psign = T(1);

	        T s2 = psign * T(2) * (psign*w*Q[A2] + Q[A1]*Q[A3]);

	        if (s2 < T(-1) + VConstants<T>::SingularityRadius)
	        { // South pole singularity
	            *a = T(0);
	            *b = -S*D*VConstants<T>::Pi/2.0;
	            *c = S*D*atan2(T(2)*(psign*Q[A1]*Q[A2] + w*Q[A3]),
			                   ww + Q22 - Q11 - Q33 );
	        }
	        else if (s2 > T(1) - VConstants<T>::SingularityRadius)
	        {  // North pole singularity
	            *a = T(0);
	            *b = S*D*VConstants<T>::Pi/2.0;
	            *c = S*D*atan2(T(2)*(psign*Q[A1]*Q[A2] + w*Q[A3]),
			                   ww + Q22 - Q11 - Q33);
	        }
	        else
	        {
	            *a = -S*D*atan2(T(-2)*(w*Q[A1] - psign*Q[A2]*Q[A3]),
			                    ww + Q33 - Q11 - Q22);
	            *b = S*D*asin(s2);
	            *c = S*D*atan2(T(2)*(w*Q[A3] - psign*Q[A1]*Q[A2]),
			                   ww + Q11 - Q22 - Q33);
	        }
	        return;
	    }

	    template <VAxis A1, VAxis A2, VAxis A3, VRotateDirection D>
	    void GetEulerAngles(T *a, T *b, T *c) const
	    { GetEulerAngles<A1, A2, A3, D, VHanded_R>(a, b, c); }

	    template <VAxis A1, VAxis A2, VAxis A3>
	    void GetEulerAngles(T *a, T *b, T *c) const
	    { GetEulerAngles<A1, A2, A3, VRotate_CCW, VHanded_R>(a, b, c); }


	    // GetEulerVAnglesABA extracts Euler VAngles from the VQuaternion, in the specified order of
	    // VAxis rotations and the specified coordinate system. Right-handed coordinate system
	    // is the default, with CCW rotations while looking in the negative VAxis diVRection.
	    // Here a,b,c, are the Yaw/Pitch/Roll VAngles to be returned.
	    // rotation a around VAxis A1
	    // is followed by rotation b around VAxis A2
	    // is followed by rotation c around VAxis A1
	    // Rotations are CCW or CW (D) in LH or RH coordinate system (S)
	    template <VAxis A1, VAxis A2, VRotateDirection D, VHandedSystem S>
	    void GetEulerAnglesABA(T *a, T *b, T *c) const
	    {
	        OVR_COMPILER_ASSERT(A1 != A2);

	        T Q[3] = {x, y, z}; // VQuaternion components

	        // Determine the missing VAxis that was not supplied
	        int m = 3 - A1 - A2;

	        T ww = w*w;
	        T Q11 = Q[A1]*Q[A1];
	        T Q22 = Q[A2]*Q[A2];
	        T Qmm = Q[m]*Q[m];

	        T psign = T(-1);
	        if ((A1 + 1) % 3 == A2) // Determine whether even permutation
	        {
	            psign = T(1);
	        }

	        T c2 = ww + Q11 - Q22 - Qmm;
	        if (c2 < T(-1) + VConstants<T>::SingularityRadius)
	        { // South pole singularity
	            *a = T(0);
	            *b = S*D*VConstants<T>::Pi;
	            *c = S*D*atan2( T(2)*(w*Q[A1] - psign*Q[A2]*Q[m]),
			                    ww + Q22 - Q11 - Qmm);
	        }
	        else if (c2 > T(1) - VConstants<T>::SingularityRadius)
	        {  // North pole singularity
	            *a = T(0);
	            *b = T(0);
	            *c = S*D*atan2( T(2)*(w*Q[A1] - psign*Q[A2]*Q[m]),
			                   ww + Q22 - Q11 - Qmm);
	        }
	        else
	        {
	            *a = S*D*atan2( psign*w*Q[m] + Q[A1]*Q[A2],
			                   w*Q[A2] -psign*Q[A1]*Q[m]);
	            *b = S*D*acos(c2);
	            *c = S*D*atan2( -psign*w*Q[m] + Q[A1]*Q[A2],
			                   w*Q[A2] + psign*Q[A1]*Q[m]);
	        }
	        return;
	    }

		bool	IsNaN( ) const { return x != x || y != y || z != z || w != w; }
};

// allow multiplication in order vector * VQuat (member operator handles VQuat * vector)
template<class T>
V3Vect<T> operator* ( const V3Vect<T> & v, const VQuat<T> & q )
{
    return V3Vect<T>( q * v );
}

typedef VQuat<float>  VQuatf;
typedef VQuat<double> VQuatd;


//-------------------------------------------------------------------------------------
// ***** VPos

// Position and orientation combined.

template<class T>
class VPos
{
public:

    typedef typename VCompatibleTypes<VPos<T> >::Type VCompatibleType;

    VPos() { }
    VPos(const VQuat<T>& orientation, const V3Vect<T>& pos)
        : Orientation(orientation), Position(pos) {  }
    VPos(const VPos& s)
        : Orientation(s.Orientation), Position(s.Position) {  }
    VPos(const VCompatibleType& s)
        : Orientation(s.Orientation), Position(s.Position) {  }
    explicit VPos(const VPos<typename VConstants<T>::VdifFloat> &s)
        : Orientation(s.Orientation), Position(s.Position) {  }

    operator const typename VCompatibleTypes<VPos<T> >::Type () const
    {
        typename VCompatibleTypes<VPos<T> >::Type result;
        result.Orientation = Orientation;
        result.Position = Position;
        return result;
    }

    V3Vect<T> Rotate(const V3Vect<T>& v) const
    {
        return Orientation.Rotate(v);
    }

    V3Vect<T> Translate(const V3Vect<T>& v) const
    {
        return v + Position;
    }

    V3Vect<T> Apply(const V3Vect<T>& v) const
    {
        return Translate(Rotate(v));
    }

    VPos operator*(const VPos& other) const
    {
        return VPos(Orientation * other.Orientation, Apply(other.Position));
    }

    VPos Inverted() const
    {
        VQuat<T> inv = Orientation.Inverted();
        return VPos(inv, inv.Rotate(-Position));
    }

    VQuat<T>    Orientation;
    V3Vect<T> Position;
};

typedef VPos<float>  VPosf;
typedef VPos<double> VPosd;

//-------------------------------------------------------------------------------------



}
